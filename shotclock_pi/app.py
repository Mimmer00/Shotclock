from flask import Flask, jsonify, request, render_template
from timer import ShotClock
from lora_sender import LoraSender

app = Flask(__name__)
clock  = ShotClock()
sender = LoraSender(clock.event_queue)

@app.route('/')
def index():
    s = clock.status()
    return render_template('index.html',
                           state=s['state'],
                           seconds=s['seconds'],
                           max_seconds=s['max'])

@app.route('/status')
def status():
    s = clock.status()
    return jsonify({
        'state':     s['state'],
        'seconds':   s['seconds'],
        'max':       s['max'],
        'connected': True,
    })

@app.route('/cmd', methods=['POST'])
def cmd():
    c = request.json.get('cmd', '').strip().upper()
    if c in ('START', 'STOP', 'RESET') or c.startswith('SETMAX '):
        clock.cmd(c)
    return jsonify({'ok': True, **clock.status()})

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=80, debug=False)
