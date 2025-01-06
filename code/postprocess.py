# postprocess.py
import time
import cv2
import os
from concurrent.futures import ThreadPoolExecutor, as_completed
import threading

LAST_FRAME = None

class RateLimiter:
    def __init__(self, interval=5.0):
        self.interval = interval
        self.last_save_time = {}

    def should_save(self, label):
        current_time = time.time()
        if label not in self.last_save_time:
            self.last_save_time[label] = current_time
            return True
        if (current_time - self.last_save_time[label]) >= self.interval:
            self.last_save_time[label] = current_time
            return True
        return False

# We'll import STREAM_READY from run_live_inference if needed
import sys
STREAM_READY = None
for name, module in sys.modules.items():
    if "run_live_inference" in name:
        try:
            STREAM_READY = getattr(module, "STREAM_READY", None)
        except:
            pass

def process_input_source(input_source, detector, cfg, rl, output_path, save_detections, results_storage):
    """
    Reads frames from input_source, runs 'detector.detect', displays with draw_boxes,
    saves detections if requested, and appends results to 'results_storage' for Flask.
    Automatically attempts to re-connect if the stream fails or times out.
    """
    global LAST_FRAME

    while True:
        # Attempt to open the stream
        cap = cv2.VideoCapture(input_source)
        if not cap.isOpened():
            print(f"[WARNING] Unable to open video source {input_source}. Will retry in 5s.")
            cap.release()
            time.sleep(5)
            continue

        print(f"[INFO] Successfully opened {input_source}. Starting frame loop...")

        # We'll track whether we've seen our first valid frame
        first_frame_received = False

        while True:
            ret, frame = cap.read()
            if not ret:
                # Possibly a network dropout or "stream timeout triggered"
                print(f"[WARNING] Lost stream from {input_source}. Will attempt reconnect in 5s.")
                cap.release()
                time.sleep(5)
                break  # Break from inner loop -> tries outer loop again

            # If we haven't yet signaled that we're ready, do so upon the first valid frame
            if not first_frame_received:
                first_frame_received = True
                if STREAM_READY is not None and not STREAM_READY.is_set():
                    print("[DEBUG] Setting STREAM_READY now (first valid frame).")
                    STREAM_READY.set()

            # Detection
            detections = detector.detect(frame)
            frame_drawn = draw_boxes(frame.copy(), detections)

            # Show local window (if desired)
            cv2.imshow(f"Detections - {input_source}", frame_drawn)

            # Update LAST_FRAME for the Flask feed
            LAST_FRAME = frame_drawn.copy()

            # Convert numpy -> Python types for storage
            for det in detections:
                if hasattr(det["bbox"], "tolist"):
                    det["bbox"] = det["bbox"].tolist()
                det["score"] = float(det["score"])

            # Add to results_storage
            if detections:
                results_storage.append({
                    "source": input_source,
                    "timestamp": time.time(),
                    "detections": detections
                })

            # Save detections if requested
            if save_detections:
                for det in detections:
                    label_name = det["label_name"]
                    if rl and not rl.should_save(label_name):
                        continue
                    save_detection(frame, det["bbox"], det["score"], label_name, output_path)

            # If user presses 'q', exit loop and stop processing this source
            if cv2.waitKey(1) & 0xFF == ord('q'):
                print(f"[INFO] User pressed 'q'. Stopping capture on {input_source}.")
                cap.release()
                cv2.destroyAllWindows()
                return  # Exit the function entirely

    # End while True (outer)
    # We'll keep looping until user quits or the program is stopped.

def run_concurrent_processing(input_sources, detector, cfg, rl, output_path, save_detections, results_storage):
    """
    Spawns threads to process each input source in parallel. We pass 'results_storage'
    so each source can store detection data for the Flask server.
    """
    if not input_sources:
        print("[run_concurrent_processing] No input sources. Nothing to do.")
        return

    with ThreadPoolExecutor(max_workers=len(input_sources)) as executor:
        futures = [
            executor.submit(
                process_input_source,
                src, detector, cfg, rl, output_path, save_detections,
                results_storage
            ) for src in input_sources
        ]
        for future in as_completed(futures):
            try:
                future.result()
            except Exception as e:
                print(f"Error processing input source: {e}")

def save_detection(image, bbox, score, label_name, output_path):
    x_min, y_min, x_max, y_max = map(int, bbox)
    cropped_img = image[y_min:y_max, x_min:x_max]

    filename = f"{label_name}_{score:.2f}.jpg"
    filepath = os.path.join(output_path, filename)

    cv2.imwrite(filepath, cropped_img)

def draw_boxes(image, detections):
    for det in detections:
        bbox = det["bbox"]
        score = det["score"]
        label_name = det["label_name"]

        # Draw rectangle
        start_point = (int(bbox[0]), int(bbox[1]))
        end_point = (int(bbox[2]), int(bbox[3]))
        color = (0, 255, 0)
        thickness = 2
        cv2.rectangle(image, start_point, end_point, color, thickness)

        # Put label text
        text = f"{label_name}: {score:.2f}"
        font = cv2.FONT_HERSHEY_SIMPLEX
        font_scale = 0.5
        text_size = cv2.getTextSize(text, font, font_scale, thickness)[0]
        text_origin = (start_point[0], start_point[1] - 10)
        cv2.rectangle(
            image,
            (text_origin[0], text_origin[1] - text_size[1] - 5),
            (text_origin[0] + text_size[0], text_origin[1] + 5),
            color, cv2.FILLED
        )
        cv2.putText(image, text, text_origin, font, font_scale, (0, 0, 0), thickness)
    return image
