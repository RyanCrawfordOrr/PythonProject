import socket
import time
import yaml
import threading
import cv2
from flask import Flask, jsonify, render_template, Response, request

import torch
from torchvision.models.detection import RetinaNet_ResNet50_FPN_Weights

# Local modules
from core_detector import Detector
from postprocess import RateLimiter, save_detection, draw_boxes

########################################################################
# GLOBALS / SHARED STATE
########################################################################

DISCOVERED_ARDUINO_IPS = []  # e.g. ["192.168.1.50", "192.168.1.51", ...]
DETECTION_THREADS = {}       # Map { ip_str: Thread() } so we don't spawn duplicates
LOCK = threading.Lock()

SHARED_FRAMES_DICT = {}      # { "http://192.168.1.50/": <last_frame_np> }

# We keep a "full" storage (if you want a complete log):
RESULTS_STORAGE = []

# We keep a "rolling" list of only the 50 most recent detections:
RECENT_DETECTIONS = []

HANDSHAKE_SOCKET = None
STREAM_READY = threading.Event()

########################################################################
# UTILITY FUNCTIONS
########################################################################

def get_local_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        return s.getsockname()[0]
    finally:
        s.close()

########################################################################
# HANDSHAKE LISTENER (Continuous)
########################################################################

def handshake_listener_continuous(udp_port=4210):
    my_ip = get_local_ip()
    print(f"[HandshakeListener] PC local IP = {my_ip}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", udp_port))

    global HANDSHAKE_SOCKET
    HANDSHAKE_SOCKET = sock

    print(f"[HandshakeListener] Listening continuously on UDP port {udp_port}...")

    while True:
        try:
            data, addr = sock.recvfrom(1024)
            message = data.decode("utf-8", errors="ignore").strip()
            sender_ip, sender_port = addr

            if message.startswith("UNO_R4 IP:"):
                possible_ip = message.split("UNO_R4 IP:")[1].strip()
                if len(possible_ip.split(".")) == 4:
                    print(f"[HandshakeListener] Received Arduino IP: {possible_ip}")

                    # Respond once
                    handshake_msg = f"PC IP:{my_ip}"
                    try:
                        sock.sendto(handshake_msg.encode("utf-8"), (possible_ip, udp_port))
                        print(f"[HandshakeListener] Sent '{handshake_msg}' to {possible_ip}:{udp_port}")
                    except Exception as e:
                        print(f"[HandshakeListener] Could not send handshake back: {e}")

                    # Now handle newly discovered IP
                    with LOCK:
                        if possible_ip not in DISCOVERED_ARDUINO_IPS:
                            DISCOVERED_ARDUINO_IPS.append(possible_ip)
                            print(f"[HandshakeListener] NEW Arduino discovered: {possible_ip}")
                            spawn_detection_thread_for_ip(possible_ip)
        except Exception as ex:
            print(f"[HandshakeListener] Error while reading from socket: {ex}")
            time.sleep(1)

########################################################################
# DETECTION THREAD SPAWNER
########################################################################

def spawn_detection_thread_for_ip(arduino_ip):
    if arduino_ip in DETECTION_THREADS:
        print(f"[spawn_detection_thread_for_ip] Thread for {arduino_ip} already running.")
        return

    t = threading.Thread(
        target=process_stream_loop,
        args=(arduino_ip,),
        daemon=True
    )
    DETECTION_THREADS[arduino_ip] = t
    t.start()
    print(f"[spawn_detection_thread_for_ip] Spawned new thread for {arduino_ip}.")

########################################################################
# THE STREAM PROCESSING LOOP
########################################################################

def process_stream_loop(arduino_ip):
    """
    Opens the MJPEG feed for the specified arduino_ip, loops over frames,
    runs detection, saves the last frame in SHARED_FRAMES_DICT, and appends
    detection results to our global structures.
    """
    mjpeg_url = f"http://{arduino_ip}/"
    print(f"[process_stream_loop] Starting detection loop for {arduino_ip}: {mjpeg_url}")

    global DETECTOR, RL, CFG, SAVE_DETECTIONS, OUTPUT_PATH

    fail_count = 0
    max_fail_threshold = 5

    while True:
        cap = None
        try:
            cap = cv2.VideoCapture(mjpeg_url)
            if not cap.isOpened():
                fail_count += 1
                print(f"[process_stream_loop:{arduino_ip}] Cannot open stream. Fail count={fail_count}")
                if fail_count > max_fail_threshold:
                    remove_arduino(arduino_ip)
                    return
                time.sleep(5)
                continue

            print(f"[process_stream_loop:{arduino_ip}] Stream opened successfully.")
            fail_count = 0

            while True:
                ret, frame = cap.read()
                if not ret:
                    fail_count += 1
                    print(f"[process_stream_loop:{arduino_ip}] Frame read failed. Fail count={fail_count}")
                    if fail_count > max_fail_threshold:
                        remove_arduino(arduino_ip)
                        return
                    time.sleep(5)
                    break  # go back, re-init cap

                fail_count = 0

                # Set the STREAM_READY event if not already set
                if not STREAM_READY.is_set():
                    STREAM_READY.set()

                # 1) Run detection
                detections = DETECTOR.detect(frame)

                # 2) Debug print: show how many detections we got
                if detections:
                    print(f"[DEBUG] {arduino_ip} => Found {len(detections)} objects.")
                    for d in detections:
                        print(f"[DEBUG]   - {d['label_name']}: {float(d['score']):.2f}")

                # 3) Draw bounding boxes
                frame_drawn = draw_boxes(frame.copy(), detections)

                # 4) Store the last frame
                SHARED_FRAMES_DICT[mjpeg_url] = frame_drawn.copy()

                # 5) Convert detection data to Python types
                for det in detections:
                    if hasattr(det["bbox"], "tolist"):
                        det["bbox"] = det["bbox"].tolist()
                    det["score"] = float(det["score"])

                # 6) Append detection results
                if detections:
                    record = {
                        "source": mjpeg_url,
                        "timestamp": time.time(),
                        "detections": detections
                    }
                    RESULTS_STORAGE.append(record)
                    add_recent_detection(record)

                # 7) Optionally save detections to disk
                if SAVE_DETECTIONS:
                    for det in detections:
                        label_name = det["label_name"]
                        if RL and not RL.should_save(label_name):
                            continue
                        save_detection(frame, det["bbox"], det["score"], label_name, OUTPUT_PATH)

                # Local "press q" to quit
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    print(f"[process_stream_loop:{arduino_ip}] 'q' pressed, stopping.")
                    remove_arduino(arduino_ip)
                    return

        except Exception as e:
            fail_count += 1
            print(f"[process_stream_loop:{arduino_ip}] Error: {e}. Fail count={fail_count}")
            if fail_count > max_fail_threshold:
                remove_arduino(arduino_ip)
                return
            time.sleep(5)
        finally:
            if cap:
                cap.release()

def remove_arduino(arduino_ip):
    """
    Removes the Arduino from the feed list, detection threads, and
    shared frames so it no longer appears on the web page.
    """
    print(f"[remove_arduino] Removing {arduino_ip} from feed list.")
    mjpeg_url = f"http://{arduino_ip}/"
    with LOCK:
        if arduino_ip in DISCOVERED_ARDUINO_IPS:
            DISCOVERED_ARDUINO_IPS.remove(arduino_ip)
        if arduino_ip in DETECTION_THREADS:
            DETECTION_THREADS.pop(arduino_ip, None)
        if mjpeg_url in SHARED_FRAMES_DICT:
            SHARED_FRAMES_DICT.pop(mjpeg_url, None)

def add_recent_detection(detection_record):
    """
    Add the detection record to RECENT_DETECTIONS (a rolling list of 50).
    If size > 50, remove the oldest.
    """
    global RECENT_DETECTIONS
    RECENT_DETECTIONS.append(detection_record)
    if len(RECENT_DETECTIONS) > 50:
        RECENT_DETECTIONS.pop(0)
    print(f"[DEBUG] Now {len(RECENT_DETECTIONS)} recent detections stored.")

########################################################################
# FLASK WEB SERVER
########################################################################

app = Flask(__name__)

@app.route('/')
def index():
    """
    The main page:
      - Displays all discovered Arduino streams in an HTML table.
      - Creates a placeholder <div> for the "recent detections".
      - Includes JavaScript that periodically fetches /api/recent_detections
        and updates the detection list (newest at the top).
    """
    with LOCK:
        ip_list = list(DISCOVERED_ARDUINO_IPS)

    html_parts = []
    html_parts.append("""<!DOCTYPE html>
<html>
<head>
  <title>Arduino Streams</title>
  <style>
    table, th, td { border: 1px solid black; border-collapse: collapse; }
    th, td { padding: 6px; }
    #recentDetections {
      white-space: pre;
      font-family: monospace;
      background: #f5f5f5;
      padding: 10px;
      width: 80%;
      max-height: 400px;
      overflow-y: auto;
    }
  </style>
</head>
<body>
""")

    html_parts.append("<h2>Arduino Streams</h2>")
    html_parts.append("<table>")
    html_parts.append("<tr><th>Arduino IP</th><th>Live Feed</th></tr>")

    for idx, ip in enumerate(ip_list):
        feed_url = f"/video_feed/{idx}"
        html_parts.append("<tr>")
        html_parts.append(f"<td>{ip}</td>")
        html_parts.append(f"<td><img src='{feed_url}' width='320'></td>")
        html_parts.append("</tr>")
    html_parts.append("</table>")

    # A section to show the newest detections:
    html_parts.append("<hr/>")
    html_parts.append("<h3>50 Most Recent Detections (Newest First)</h3>")
    # We'll dynamically insert content into this div via AJAX
    html_parts.append("<div id='recentDetections'>Loading...</div>")

    # JS script to auto-refresh the detection list
    html_parts.append("""
<script>
function fetchDetections() {
  fetch('/api/recent_detections')
    .then(response => response.json())
    .then(data => {
      // data is an array of detection records
      // We'll build a text block, newest first
      let textContent = "";
      data.forEach((record, idx) => {
        // record: { "timestamp": 12345.67, "source": "...", "detections": [ {...}, {...} ] }
        let dateStr = new Date(record.timestamp * 1000).toLocaleString();
        let src = record.source;
        let detObjs = record.detections;
        detObjs.forEach(det => {
          textContent += dateStr + " | " + src + " | " + det.label_name + ":" + det.score.toFixed(2) + "\\n";
        });
      });
      document.getElementById('recentDetections').textContent = textContent;
    })
    .catch(err => {
      console.error('Error fetching /api/recent_detections:', err);
    });
}

// Poll every 2 seconds
setInterval(fetchDetections, 2000);
// Fetch once immediately
fetchDetections();
</script>
""")

    html_parts.append("</body></html>")
    return "\n".join(html_parts)

@app.route('/api/recent_detections')
def api_recent_detections():
    """
    Returns JSON of the last 50 detections, with the most recent first.
    We reverse the list so the newest is at index 0 of the returned array.
    """
    with LOCK:
        # Make a shallow copy, reverse it
        reversed_list = list(reversed(RECENT_DETECTIONS))
    return jsonify(reversed_list)

@app.route('/video_feed/<int:source_id>')
def video_feed(source_id):
    with LOCK:
        if source_id < 0 or source_id >= len(DISCOVERED_ARDUINO_IPS):
            return f"No such source {source_id}", 404
        ip = DISCOVERED_ARDUINO_IPS[source_id]
    mjpeg_url = f"http://{ip}/"
    return Response(
        generate_frames(mjpeg_url),
        mimetype='multipart/x-mixed-replace; boundary=frame'
    )

def generate_frames(source_key):
    while True:
        frame_to_stream = SHARED_FRAMES_DICT.get(source_key, None)
        if frame_to_stream is not None:
            ret, buffer = cv2.imencode('.jpg', frame_to_stream)
            if ret:
                frame_bytes = buffer.tobytes()
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')
        time.sleep(0.05)  # ~20 FPS

########################################################################
# MAIN
########################################################################

def main():
    global DETECTOR, RL, CFG, SAVE_DETECTIONS, OUTPUT_PATH

    config_path = "./configs/config.yaml"
    with open(config_path, 'r') as f:
        CFG = yaml.safe_load(f)

    device = "cuda" if torch.cuda.is_available() else "cpu"
    weights_map = {
        "COCO_V1": RetinaNet_ResNet50_FPN_Weights.COCO_V1,
        "DEFAULT": RetinaNet_ResNet50_FPN_Weights.DEFAULT
    }
    chosen_weights = weights_map[CFG["model"]["pretrained"]]

    DETECTOR = Detector(
        confidence_threshold=CFG["model"]["confidence_threshold"],
        weights=chosen_weights,
        device=device
    )

    RL = None
    if CFG["rate_limiter"]["enable"]:
        RL = RateLimiter(CFG["rate_limiter"]["interval_seconds"])

    SAVE_DETECTIONS = CFG["pipeline"]["save_detections"]
    OUTPUT_PATH = CFG["pipeline"]["output_path"]

    # Start continuous handshake listener
    listener_thread = threading.Thread(
        target=handshake_listener_continuous, args=(4210,), daemon=True
    )
    listener_thread.start()

    # Start the Flask server
    local_ip = get_local_ip()
    print(f"[Main] Web server available at: http://{local_ip}:5000/")
    app.run(host='0.0.0.0', port=5000, debug=False)


if __name__ == "__main__":
    main()
