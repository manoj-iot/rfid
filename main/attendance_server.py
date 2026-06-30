import os
import json
import threading
from datetime import datetime
from flask import Flask, render_template, request, jsonify, Response

# ── Initialize Flask App ─────────────────────────────────────────────────────
# Templates live in: <project_root>/web/templates/
# Static files live in: <project_root>/web/static/
_BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project root
app = Flask(
    __name__,
    template_folder=os.path.join(_BASE_DIR, 'web', 'templates'),
    static_folder=os.path.join(_BASE_DIR, 'web', 'static'),
)

# ── Constants ────────────────────────────────────────────────────────────────
# Database lives in: <project_root>/data/attendance_db.json
DB_FILE = os.path.join(_BASE_DIR, 'data', 'attendance_db.json')

# Thread-safe lists for SSE clients and database locks
sse_clients = []
db_lock = threading.Lock()

# Load/Initialize Database
def load_db():
    with db_lock:
        if not os.path.exists(DB_FILE):
            default_data = {
                "users": {
                    "0D C2 18 06": "Manoj" # Register user's card by default
                },
                "logs": []
            }
            with open(DB_FILE, 'w') as f:
                json.dump(default_data, f, indent=4)
            return default_data
        try:
            with open(DB_FILE, 'r') as f:
                return json.load(f)
        except Exception:
            return {"users": {}, "logs": []}

def save_db(data):
    with db_lock:
        with open(DB_FILE, 'w') as f:
            json.dump(data, f, indent=4)

# Determine scan type (Check-In vs Check-Out)
def determine_scan_type(uid):
    db = load_db()
    logs = db.get("logs", [])
    today_str = datetime.now().strftime("%Y-%m-%d")
    
    # Filter logs for this UID scanned today
    today_logs = [log for log in logs if log.get("uid") == uid and log.get("timestamp", "").startswith(today_str)]
    
    # Alternate Check-In and Check-Out
    if len(today_logs) % 2 == 0:
        return "Check-In"
    else:
        return "Check-Out"

# Process card scan from Wi-Fi HTTP client
def register_scan(uid):
    db = load_db()
    users = db.get("users", {})
    name = users.get(uid, "Unknown Profile")
    scan_type = determine_scan_type(uid)
    timestamp = datetime.now().strftime("%Y-%m-%d %I:%M:%S %p")
    
    new_log = {
        "uid": uid,
        "name": name,
        "timestamp": timestamp,
        "type": scan_type
    }
    
    db["logs"].insert(0, new_log) # Add to start of list
    save_db(db)
    
    # Broadcast to all connected web clients via SSE
    payload = json.dumps(new_log)
    for client in sse_clients:
        client.put(payload)
        
    print(f"[SCAN LOGGED OVER WI-FI] UID: {uid} | Name: {name} | Type: {scan_type} | Time: {timestamp}")

# Flask APIs
@app.route('/api/logs', methods=['GET'])
def get_logs():
    db = load_db()
    return jsonify(db.get("logs", []))

@app.route('/api/users', methods=['GET'])
def get_users():
    db = load_db()
    return jsonify(db.get("users", {}))

# Wi-Fi HTTP POST Endpoint for ESP32
@app.route('/api/scan', methods=['POST'])
def api_scan():
    print(f"[API SCAN] Received request data: {request.data}")
    try:
        req = request.get_json()
        if not req:
            print("[API SCAN] Failed to parse JSON request data.")
            return jsonify({"success": False, "error": "Invalid JSON"}), 400
        uid = req.get("uid")
        if not uid:
            print("[API SCAN] Missing UID in request.")
            return jsonify({"success": False, "error": "Missing UID"}), 400
        register_scan(uid)
        print(f"[API SCAN] Successfully registered scan for UID: {uid}")
        return jsonify({"success": True})
    except Exception as e:
        import traceback
        print(f"[API SCAN] Error processing scan request: {e}")
        traceback.print_exc()
        return jsonify({"success": False, "error": str(e)}), 500

@app.route('/api/register', methods=['POST'])
def register_user():
    req = request.get_json()
    uid = req.get("uid")
    name = req.get("name")
    if not uid or not name:
        return jsonify({"success": False, "error": "Missing UID or Name"}), 400
        
    db = load_db()
    db["users"][uid] = name
    
    # Retroactively update names in past logs for this UID
    for log in db.get("logs", []):
        if log.get("uid") == uid:
            log["name"] = name
            
    save_db(db)
    return jsonify({"success": True})

# Edit existing student name (reassign card)
@app.route('/api/edit_student', methods=['POST'])
def edit_student():
    req = request.get_json()
    uid = req.get('uid')
    new_name = req.get('new_name')
    if not uid or not new_name:
        return jsonify({'success': False, 'error': 'Missing uid or new_name'}), 400
    db = load_db()
    if uid not in db.get('users', {}):
        return jsonify({'success': False, 'error': 'UID not found'}), 404
    db['users'][uid] = new_name
    # Update past logs
    for log in db.get('logs', []):
        if log.get('uid') == uid:
            log['name'] = new_name
    save_db(db)
    return jsonify({'success': True})

# SSE Endpoint for Live Updates
import queue
@app.route('/api/live-stream')
def sse_stream():
    def event_generator():
        q = queue.Queue()
        sse_clients.append(q)
        try:
            # Send initial ping
            yield "data: {\"type\": \"ping\"}\n\n"
            while True:
                data = q.get()
                yield f"data: {data}\n\n"
        except GeneratorExit:
            sse_clients.remove(q)
            
    return Response(event_generator(), mimetype="text/event-stream")

# ── Frontend Web Dashboard Route ────────────────────────────────────────────
@app.route('/')
def index():
    return render_template('dashboard.html')



# ── Main Application Entry ───────────────────────────────────────────────────
if __name__ == '__main__':
    print("Launching Local Web Server on http://localhost:5000...")
    app.run(host='0.0.0.0', port=5000, debug=False)
