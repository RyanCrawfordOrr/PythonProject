"""
run_live_inference_2.py
--------------------------------
- No local input sources: we rely 100% on discovering Arduino(s).
- Performs a continuous handshake with the Arduino(s):
    - A background thread listens for "UNO_R4 IP:<ArduinoIP>" on UDP 4210 forever.
    - Whenever it receives an Arduino IP, it replies "PC IP:<LocalPCIP>" once,
      and updates the global DISCOVERED_ARDUINO_IP.
- A dedicated thread continuously attempts to open the MJPEG feed from the discovered Arduino IP.
  If it fails, it sleeps and tries again indefinitely.
- (Optional) starts a Flask web server to show results, including a live bounding-box feed at
  /video_feed so you can embed <img src="/video_feed"> in a webpage.
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
    run_concurrent_processing,  # We only use it if we had local sources, but we can keep it imported
    LAST_FRAME,
)

########################################################################
# Globals / Shared State
########################################################################

DISCOVERED_ARDUINO_IP = None
LOCK = threading.Lock()

# This event can be used to signal the Flask generator that frames are ready
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

def handshake_listener_continuous(udp_port=4210):
    """
    Continuously listens on udp_port for ANY "UNO_R4 IP:<ArduinoIP>" broadcast.
    Each time we get one:
      1) Respond with "PC IP:<LocalPCIP>"
      2) Update DISCOVERED_ARDUINO_IP in global
    """
    my_ip = get_local_ip()
    print(f"[HandshakeListener] PC local IP = {my_ip}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", udp_port))

    print(f"[HandshakeListener] Listening for broadcasts on UDP port {udp_port}...")

    while True:
        try:
            data, addr = sock.recvfrom(1024)
            message = data.decode("utf-8", errors="ignore").strip()
            sender_ip, sender_port = addr

            # e.g. "UNO_R4 IP:192.168.1.50"
            if message.startswith("UNO_R4 IP:"):
                possible_ip = message.split("UNO_R4 IP:")[1].strip()
                # Very basic IP check (just ensure x.x.x.x form)
                if len(possible_ip.split(".")) == 4:
                    print(f"[HandshakeListener] Received Arduino IP '{possible_ip}' from {sender_ip}:{sender_port}")

                    # Respond once
                    handshake_msg = f"PC IP:{my_ip}"
                    try:
                        sock.sendto(handshake_msg.encode("utf-8"), (possible_ip, udp_port))
                        print(f"[HandshakeListener] Sent '{handshake_msg}' to {possible_ip}:{udp_port}")
                    except Exception as e:
                        print(f"[HandshakeListener] Could not send handshake back: {e}")

                    # Store discovered IP in global
                    with LOCK:
                        global DISCOVERED_ARDUINO_IP
                        DISCOVERED_ARDUINO_IP = possible_ip

        except Exception as ex:
            print(f"[HandshakeListener] Error while reading from socket: {ex}")
            time.sleep(1)  # Avoid spamming in a tight loop if there's an error

########################################################################
# A Dedicated Function to Process the Arduino Stream
########################################################################

def process_arduino_stream(detector, cfg, rl, output_path, save_detections, results_storage):
    """
    Continuously attempts to:
      1) Check if Arduino IP is discovered. If not, sleep and retry.
      2) Open the MJPEG stream from the discovered IP.
      3) Read frames, run detection, store bounding-box visuals in LAST_FRAME.
      4) If the feed breaks, close and retry.
      5) If we never discover an IP, we just keep sleeping until we do.
    """
    global LAST_FRAME  # We will assign to LAST_FRAME in this thread

    while True:
        # 1) Check if we have an Arduino IP
        with LOCK:
            ip = DISCOVERED_ARDUINO_IP

        if not ip:
            print("[process_arduino_stream] No Arduino IP discovered yet. Sleeping...")
            time.sleep(3)
            continue

        source_url = f"http://{ip}/"
        print(f"[process_arduino_stream] Attempting to connect to {source_url}")
        cap = cv2.VideoCapture(source_url)
        if not cap.isOpened():
            print(f"[process_arduino_stream] Could not open {source_url}, will retry...")
            cap.release()
            time.sleep(3)
            continue

        print(f"[process_arduino_stream] Connected to {source_url}!")

        # If you want ~20 FPS, adjust as needed
        desired_delay = 1.0 / 20.0

        while True:
            start_time = time.time()
            ret, frame = cap.read()
            if not ret or frame is None:
                print(f"[process_arduino_stream] Lost connection to {source_url}. Re-opening soon...")
                cap.release()
                break

            # Optional rate limit
            if rl is not None and not rl.allow_processing():
                continue

            # Run detection
            detections = detector.detect(frame)

            # Update global LAST_FRAME with bounding-box drawing or raw frame
            if LAST_FRAME is None:
                # First time we set it
                LAST_FRAME = frame.copy()
            else:
                # We can now safely copy in place
                LAST_FRAME[:] = frame[:]

            # Optionally store detections
            if save_detections:
                results_storage.append(detections)

            # Aim for ~20 FPS
            elapsed = time.time() - start_time
            if elapsed < desired_delay:
                time.sleep(desired_delay - elapsed)

        # If we reach here, the feed broke; try again after a short sleep
        time.sleep(3)

########################################################################
# Functions for Serving the Live Video Feed via Flask
########################################################################

def generate_frames():
    """
    A generator function that yields MJPEG frames from LAST_FRAME.
    If bounding-box drawing is performed in postprocess or during detection,
    LAST_FRAME should already contain those bounding-box overlays.
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
        # ~20 FPS
        time.sleep(0.05)

def start_web_server(results_storage):
    app = Flask(__name__)

    @app.route('/')
    def index():
        # If you have an index.html template, pass in results to show a table, etc.
        return render_template('index.html', results=results_storage)

    @app.route('/api/results')
    def api_results():
        # Return detection results as JSON
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

    # 3) Start the continuous handshake listener in the background
    listener_thread = threading.Thread(
        target=handshake_listener_continuous,
        args=(4210,),
        daemon=True
    )
    listener_thread.start()

    # 4) If web_server is enabled, run it in a separate thread
    results_storage = []
    if cfg["web_server"]["enable"]:
        print("[Main] Starting the web server.")
        web_server_thread = threading.Thread(
            target=start_web_server,
            args=(results_storage,),
            daemon=True
        )
        web_server_thread.start()

    # 5) Start the dedicated Arduino stream processing thread
    output_path = cfg["pipeline"]["output_path"]
    save_detections = cfg["pipeline"]["save_detections"]

    arduino_thread = threading.Thread(
        target=process_arduino_stream,
        args=(detector, cfg, rl, output_path, save_detections, results_storage),
        daemon=True
    )
    arduino_thread.start()

    # If you ever want to process additional local sources, you can call
    # run_concurrent_processing(...) in another thread. But as we rely
    # 100% on discovering Arduino(s), we skip that here.

    # 6) Idle forever
    print("[Main] All threads started. Waiting indefinitely...")
    while True:
        time.sleep(1)

if __name__ == "__main__":
    main()
