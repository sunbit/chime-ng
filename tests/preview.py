#!/usr/bin/env python3
"""Loopback-only web UI fixture; does not talk to the board or an MQTT broker."""
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
state=dict(version='0.2.1',led=False,led_diagnostic='gpio11 requested=0 latch=0 dir=1 mode=0 ready=1 drive=20 type=0 before=820 pad=820 oe=800',led_command='seq=0 result=none',ip='192.168.4.1',web='listening:80',diagnostic='none',configured=False,ap=True,mac='02:11:22:33:44:55',mqtt='offline',playing=False,audio_ready=True,error='none',confirmed=True,ssid='',broker='',mqtt_port=1883,mqtt_user='',mqtt_tls=0,ntp_server='',homie_id='',name='',volume=25,ca=False)
class Preview(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path=='/':
            data=(Path(__file__).resolve().parents[1]/'web/en.html').read_text().replace('{{version}}', '0.2.1').encode();kind='text/html; charset=utf-8'
        elif self.path=='/api/status':data=json.dumps(state).encode();kind='application/json'
        else:self.send_error(404);return
        self.send_response(200);self.send_header('Content-Type',kind);self.send_header('Content-Length',len(data));self.end_headers();self.wfile.write(data)
    def do_POST(self):
        body=self.rfile.read(int(self.headers.get('Content-Length','0')))
        if self.path=='/api/volume':state['volume']=int(body)
        if self.path=='/api/play':state['playing']=True
        if self.path=='/api/led':
            state['led']=body==b'true'
            level=int(state['led'])
            state['led_diagnostic']=f'gpio11 requested={level} latch={level} dir=1 mode=0 ready=1 drive=20 type=0 before=820 pad=820 oe=800'
        data='Local UI preview: no request was sent to a physical chime.'.encode()
        self.send_response(200);self.send_header('Content-Length',len(data));self.end_headers();self.wfile.write(data)
    do_PUT=do_POST
if __name__=='__main__':
    print('UI fixture on http://127.0.0.1:5059',flush=True)
    HTTPServer(('127.0.0.1',5059),Preview).serve_forever()
