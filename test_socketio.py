import eventlet
eventlet.monkey_patch()
from flask import Flask
from flask_socketio import SocketIO
import socket

app = Flask(__name__)
socketio = SocketIO(app, async_mode='eventlet', cors_allowed_origins="*")

@app.route('/')
def index():
    return "Hello, World! This is a test."

if __name__ == "__main__":
    # Print the local IP for convenience
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(('8.8.8.8', 80))
    local_ip = s.getsockname()[0]
    s.close()

    print(f"TEST server at: http://{local_ip}:5000")
    socketio.run(app, host='0.0.0.0', port=5000, debug=True)
