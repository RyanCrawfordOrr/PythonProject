"""
run_live_inference.py
--------------------------------
- No local input sources: we rely 100% on discovering Arduino(s).
- Performs a one-time handshake with the first Arduino:
    1) Arduino broadcasts "UNO_R4 IP:<ArduinoIP>" on UDP 4210.
    2) Python replies "PC IP:<LocalPCIP>" once.
    3) Adds "http://<ArduinoIP>/" to the input sources for object detection.
- (Optional) starts a Flask web server to show results.
- Now includes a live bounding-box feed at /video_feed so you can embed
  <img src="/video_feed"> on the webpage.
"""

import socket
import time
import yaml
import threading
import cv2
from flask import Flask, jsonify, render_template, Response

import torch
from torchvision.models.detection import RetinaNet_ResNet50_FPN_Weights

# Your existing code references
from core_detector import Detector
from postprocess import (
    RateLimiter,
    run_concurrent_processing,
    LAST_FRAME,
)

########################################################################
# Globals / Shared State
########################################################################

DISCOVERED_ARDUINO_IP = None
LOCK = threading.Lock()
WAIT_FOR_ARDUINO_SECS = 1000  # how long main() waits for handshake to complete

# This event signals the Flask generator that frames are ready
STREAM_READY = threading.Event()

########################################################################
# Networking / Handshake Helper Functions
########################################################################

def get_local_ip() -> str:
    """
    Attempts to determine this machine's local IP address by creating
    a UDP socket to a known public IP (8.8.8.8) without actually sending data.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        return s.getsockname()[0]
    finally:
        s.close()

def handshake_listener_once(udp_port=4210):
    """
    Listen on udp_port for exactly ONE "UNO_R4 IP:<ArduinoIP>" broadcast.
    Once received, respond with "PC IP:<myLocalIP>", store the Arduino IP,
    then CLOSE the socket and exit this thread.
    """
    my_ip = get_local_ip()
    print(f"[HandshakeListener] PC local IP = {my_ip}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    # **CHANGE**: On some OSes you may also want to sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    # But usually binding to ("", udp_port) is enough to receive broadcast
    sock.bind(("", udp_port))

    print(f"[HandshakeListener] Listening for one broadcast on UDP port {udp_port}...")

    while True:
        try:
            data, addr = sock.recvfrom(1024)
            message = data.decode("utf-8", errors="ignore").strip()
            sender_ip, sender_port = addr

            # e.g. "UNO_R4 IP:192.168.1.50"
            if message.startswith("UNO_R4 IP:"):
                possible_ip = message.split("UNO_R4 IP:")[1].strip()
                # Very basic IP check
                if len(possible_ip.split(".")) == 4:
                    print(f"[HandshakeListener] Received Arduino IP '{possible_ip}' from {sender_ip}:{sender_port}")

                    # Respond once
                    handshake_msg = f"PC IP:{my_ip}"
                    try:
                        sock.sendto(handshake_msg.encode("utf-8"), (possible_ip, udp_port))
                        print(f"[HandshakeListener] Sent '{handshake_msg}' to {possible_ip}:{udp_port}")
                    except Exception as e:
                        print(f"[HandshakeListener] Could not send handshake back: {e}")

                    # Store discovered IP in global, then EXIT
                    with LOCK:
                        global DISCOVERED_ARDUINO_IP
                        DISCOVERED_ARDUINO_IP = possible_ip

                    sock.close()
                    print("[HandshakeListener] Handshake done, exiting listener thread.")
                    return

        except Exception as ex:
            print(f"[HandshakeListener] Error while reading from socket: {ex}")
            time.sleep(1)  # Just avoid spamming if there's an error

########################################################################
# (New) Functions for Serving the Live Video Feed
########################################################################

def generate_frames():
    """
    A generator function that yields MJPEG frames from LAST_FRAME.
    We rely on code in postprocess.py to keep LAST_FRAME updated
    with the bounding-box visuals.
    """
    while True:
        if LAST_FRAME is not None:
            ret, buffer = cv2.imencode('.jpg', LAST_FRAME)
            if ret:
                frame_bytes = buffer.tobytes()
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')
            else:
                print("[ERROR] Frame encoding failed.")
        time.sleep(0.05)  # ~20 FPS

########################################################################
# Optional: Start a Flask web server to display detection results
########################################################################

def start_web_server(results_storage):
    app = Flask(__name__)

    @app.route('/')
    def index():
        return render_template('index.html', results=results_storage)

    @app.route('/api/results')
    def api_results():
        return jsonify(results_storage)

    @app.route('/video_feed')
    def video_feed():
        """
        This route returns the live bounding-boxed frames as an MJPEG stream.
        """
        print("[INFO] /video_feed route accessed.")
        return Response(
            generate_frames(),
            mimetype='multipart/x-mixed-replace; boundary=frame'
        )

    local_ip = get_local_ip()
    print(f"[WebServer] Web interface available at: http://{local_ip}:5000/")
    print(f"[WebServer] Video feed available at: http://{local_ip}:5000/video_feed")

    print("[WebServer] Starting Flask on port 5000...")
    app.run(host='0.0.0.0', port=5000, debug=False)

########################################################################
# Main
########################################################################

def main():
    # 1) Load config
    config_path = "./configs/config.yaml"
    with open(config_path, 'r') as f:
        cfg = yaml.safe_load(f)

    # 2) Prepare the detector
    device = "cuda" if torch.cuda.is_available() else "cpu"
    weights_map = {
        "COCO_V1": RetinaNet_ResNet50_FPN_Weights.COCO_V1,
        "DEFAULT": RetinaNet_ResNet50_FPN_Weights.DEFAULT
    }
    chosen_weights = weights_map[cfg["model"]["pretrained"]]
    detector = Detector(
        confidence_threshold=cfg["model"]["confidence_threshold"],
        weights=chosen_weights,
        device=device
    )

    # Optional Rate Limiter
    rl = None
    if cfg["rate_limiter"]["enable"]:
        rl = RateLimiter(cfg["rate_limiter"]["interval_seconds"])

    # 3) Start handshake listener in background
    listener_thread = threading.Thread(target=handshake_listener_once, args=(4210,), daemon=True)
    listener_thread.start()

    # 4) Wait up to WAIT_FOR_ARDUINO_SECS for the Arduino IP
    print(f"[Main] Waiting up to {WAIT_FOR_ARDUINO_SECS}s for the first handshake broadcast.")
    arduino_ip = None
    start_wait = time.time()

    while True:
        with LOCK:
            if DISCOVERED_ARDUINO_IP is not None:
                arduino_ip = DISCOVERED_ARDUINO_IP
                break
        if (time.time() - start_wait) > WAIT_FOR_ARDUINO_SECS:
            break
        time.sleep(0.5)

    # 5) Build input_sources
    input_sources = []
    if arduino_ip:
        mjpeg_url = f"http://{arduino_ip}/"
        print(f"[Main] Discovered Arduino IP {arduino_ip}. Adding camera stream: {mjpeg_url}")
        input_sources.append(mjpeg_url)
    else:
        print(f"[Main] No Arduino IP discovered after {WAIT_FOR_ARDUINO_SECS}s.")
        print("[Main] There are no local sources either. Running with empty input sources.")

    output_path = cfg["pipeline"]["output_path"]
    save_detections = cfg["pipeline"]["save_detections"]

    # 6) Start Flask server in separate thread if enabled
    results_storage = []
    if cfg["web_server"]["enable"]:
        print("[Main] Starting the web server.")
        web_server_thread = threading.Thread(
            target=start_web_server,
            args=(results_storage,),
            daemon=True
        )
        web_server_thread.start()

    # 7) Start the concurrency for input streams
    print("[Main] Initializing stream processing.")

    # We'll do the concurrency in its own thread so as not to block.
    def streaming_thread():
        run_concurrent_processing(
            input_sources,
            detector,
            cfg,
            rl,
            output_path,
            save_detections,
            results_storage
        )

    processing_thread = threading.Thread(target=streaming_thread, daemon=True)
    processing_thread.start()

    # Because run_concurrent_processing() never returns (it loops forever),
    # we won't rely on that to set STREAM_READY. Instead, the first time we
    # decode a frame in postprocess.py, we will call STREAM_READY.set().
    # So we can idle here:
    while True:
        time.sleep(1)

if __name__ == "__main__":
    main()
