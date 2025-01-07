# run_live_inference_new.py
# ---------------------------------------------------------------------
# - We rely on discovering Arduino(s) for an MJPEG stream.
# - Once we have the Arduino IP, we add its MJPEG stream to input_sources.
# - We optionally launch a Flask server to serve a live bounding-box feed at /video_feed.
# - We removed 'LAST_FRAME' references. Instead, we store the latest frames in 'shared_frames_dict'.
# ---------------------------------------------------------------------

import socket
import time
import yaml
import threading
import cv2
from flask import Flask, jsonify, render_template, Response

import torch
from torchvision.models.detection import RetinaNet_ResNet50_FPN_Weights

# References to your local modules
from core_detector import Detector
from postprocess import (
    RateLimiter,
    run_concurrent_processing,
    # We do not import LAST_FRAME anymore
)

########################################################################
# Globals / Shared State
########################################################################

DISCOVERED_ARDUINO_IP = None
LOCK = threading.Lock()
WAIT_FOR_ARDUINO_SECS = 1000  # how long main() waits for handshake
STREAM_READY = threading.Event()

# We'll store a global reference to the socket for cleanup
HANDSHAKE_SOCKET = None

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
    then close the socket and exit this thread.
    """
    my_ip = get_local_ip()
    print(f"[HandshakeListener] PC local IP = {my_ip}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    sock.bind(("", udp_port))
    print(f"[HandshakeListener] Listening for one broadcast on UDP port {udp_port}...")

    global HANDSHAKE_SOCKET
    HANDSHAKE_SOCKET = sock

    while True:
        try:
            data, addr = sock.recvfrom(1024)
            message = data.decode("utf-8", errors="ignore").strip()
            sender_ip, sender_port = addr

            # e.g. "UNO_R4 IP:192.168.1.50"
            if message.startswith("UNO_R4 IP:"):
                possible_ip = message.split("UNO_R4 IP:")[1].strip()
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

                    # Close the socket
                    sock.close()
                    HANDSHAKE_SOCKET = None
                    print("[HandshakeListener] Handshake done, exiting listener thread.")
                    return
        except Exception as ex:
            print(f"[HandshakeListener] Error while reading from socket: {ex}")
            time.sleep(1)

########################################################################
# (New) Functions for Serving the Live Video Feed
########################################################################

def generate_frames(shared_frames_dict, source_key):
    """
    A generator function that yields MJPEG frames from shared_frames_dict[source_key].
    """
    while True:
        frame_to_stream = shared_frames_dict.get(source_key, None)
        if frame_to_stream is not None:
            ret, buffer = cv2.imencode('.jpg', frame_to_stream)
            if ret:
                frame_bytes = buffer.tobytes()
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')
        time.sleep(0.05)  # ~20 FPS

########################################################################
# Optional: Start a Flask web server to display detection results
########################################################################

def start_web_server(results_storage, shared_frames_dict, source_key):
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
            generate_frames(shared_frames_dict, source_key),
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
    global HANDSHAKE_SOCKET
    try:
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
        listener_thread = threading.Thread(
            target=handshake_listener_once, args=(4210,), daemon=True
        )
        listener_thread.start()

        # 4) Wait up to WAIT_FOR_ARDUINO_SECS for the Arduino IP
        print(f"[Main] Waiting up to {WAIT_FOR_ARDUINO_SECS}s for the handshake broadcast.")
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

        # 5) Build input_sources based on discovered Arduino
        input_sources = []
        if arduino_ip:
            mjpeg_url = f"http://{arduino_ip}/"
            print(f"[Main] Discovered Arduino IP {arduino_ip}. Adding camera stream: {mjpeg_url}")
            input_sources.append(mjpeg_url)
        else:
            print(f"[Main] No Arduino IP discovered after {WAIT_FOR_ARDUINO_SECS}s.")
            print("[Main] Running with empty input sources (no camera streams).")

        output_path = cfg["pipeline"]["output_path"]
        save_detections = cfg["pipeline"]["save_detections"]

        # 6) Create a shared dict for each camera feed's latest frame
        shared_frames_dict = {}  # e.g. {"http://192.168.1.50/": last_frame_np_array}

        # For storing detection results across all streams (shown on the web UI)
        results_storage = []

        # 7) Start the Flask server in a separate thread if enabled
        if cfg["web_server"]["enable"]:
            print("[Main] Starting the web server.")
            # If you have multiple input sources, you could pick one to display or add multiple endpoints
            if input_sources:
                source_key = input_sources[0]  # Just pick the first discovered Arduino
            else:
                source_key = "default"

            web_server_thread = threading.Thread(
                target=start_web_server,
                args=(results_storage, shared_frames_dict, source_key),
                daemon=True
            )
            web_server_thread.start()

        # 8) Start concurrent stream processing in a background thread
        def streaming_thread():
            run_concurrent_processing(
                input_sources,
                detector,
                cfg,
                rl,
                output_path,
                save_detections,
                results_storage,
                shared_frames_dict
            )

        processing_thread = threading.Thread(target=streaming_thread, daemon=True)
        processing_thread.start()

        # 9) Idle here
        while True:
            time.sleep(1)

    except KeyboardInterrupt:
        print("\n[Main] KeyboardInterrupt received. Attempting graceful shutdown...")

    finally:
        # If the handshake socket is still open, close it
        if HANDSHAKE_SOCKET:
            print("[Main] Closing handshake socket due to abrupt stop or script exit.")
            HANDSHAKE_SOCKET.close()
            HANDSHAKE_SOCKET = None

if __name__ == "__main__":
    main()
