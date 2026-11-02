"""Exercise the same portable C sources compiled into the firmware."""
import base64
import ctypes as C
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import unittest
import zlib

FW=Path(__file__).resolve().parents[1]
OUT=FW/'build'
subprocess.run(['clang','-std=c11','-shared','-fPIC','-O1','-Wall','-Wextra','-Werror','-I',str(FW/'include'),str(FW/'src/core.c'),str(FW/'src/sha256.c'),str(FW/'src/http_parse.c'),'-o',str(OUT/'core-tests.dylib')],check=True)
lib=C.CDLL(str(OUT/'core-tests.dylib'))
class Sha(C.Structure):
    _fields_=[('state',C.c_uint32*8),('bytes',C.c_uint64),('block',C.c_ubyte*64)]
class Image(C.Structure):
    _fields_=[('magic',C.c_char*8),('board',C.c_uint32),('format',C.c_uint32),('address',C.c_uint32),('size',C.c_uint32),('build',C.c_uint32),('flags',C.c_uint32),('sha',C.c_ubyte*32)]
class Boot(C.Structure):
    _fields_=[('magic',C.c_uint32),('active',C.c_ubyte),('pending',C.c_ubyte),('tried',C.c_ubyte),('reserved',C.c_ubyte),('generation',C.c_uint32),('crc',C.c_uint32)]
class Request(C.Structure):
    _fields_=[('method',C.c_char*8),('path',C.c_char*48),('auth',C.c_char*240),('type',C.c_char*64),('length',C.c_uint32),('length_present',C.c_bool),('custom',C.c_bool)]
class Publish(C.Structure):
    _fields_=[('topic',C.c_void_p),('payload',C.c_void_p),('topic_len',C.c_size_t),('payload_len',C.c_size_t),('qos',C.c_ubyte),('retained',C.c_bool),('duplicate',C.c_bool),('packet_id',C.c_uint16)]
lib.sha256.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p]
lib.sha256_init.argtypes=[C.POINTER(Sha)]
lib.sha256_update.argtypes=[C.POINTER(Sha),C.c_void_p,C.c_size_t]
lib.sha256_final.argtypes=[C.POINTER(Sha),C.c_void_p]
lib.crc32.argtypes=[C.c_void_p,C.c_size_t];lib.crc32.restype=C.c_uint32
for name in ['image_header_valid','image_vectors_valid','boot_record_valid','homie_id_valid','form_get','basic_password','wav_header','http_parse_headers','mqtt_parse_publish','play_command_valid','parse_uint']:
    getattr(lib,name).restype=C.c_bool
lib.image_header_valid.argtypes=[C.POINTER(Image)]
lib.image_vectors_valid.argtypes=[C.POINTER(Image),C.c_void_p]
lib.boot_record_valid.argtypes=[C.POINTER(Boot)]
lib.boot_record_seal.argtypes=[C.POINTER(Boot)]
lib.boot_select.argtypes=[C.POINTER(Boot)];lib.boot_select.restype=C.c_uint
lib.homie_id_valid.argtypes=[C.c_char_p]
lib.form_get.argtypes=[C.c_char_p,C.c_char_p,C.c_void_p,C.c_size_t]
lib.basic_password.argtypes=[C.c_char_p,C.c_void_p,C.c_size_t]
lib.wav_header.argtypes=[C.c_void_p,C.c_size_t,C.c_uint32,C.POINTER(C.c_uint32),C.POINTER(C.c_uint32)]
lib.http_parse_headers.argtypes=[C.c_void_p,C.POINTER(Request)]
lib.homie_description.argtypes=[C.c_void_p,C.c_size_t,C.c_char_p];lib.homie_description.restype=C.c_size_t
lib.mqtt_frame_size.argtypes=[C.c_void_p,C.c_size_t,C.c_size_t,C.POINTER(C.c_size_t)];lib.mqtt_frame_size.restype=C.c_int
lib.mqtt_parse_publish.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(Publish)]
lib.mqtt_varint.argtypes=[C.c_void_p,C.c_size_t];lib.mqtt_varint.restype=C.c_size_t
lib.parse_uint.argtypes=[C.c_char_p,C.c_uint32,C.POINTER(C.c_uint32)]

def image():
    return Image(b'CHIMNG01',0x43483332,1,0x20010000,2048,1,0)
def parse_request(text):
    r=Request();buf=C.create_string_buffer(text);return lib.http_parse_headers(buf,C.byref(r)),r

class Core(unittest.TestCase):
    def test_sha_and_crc_reference_vectors(self):
        rng=random.Random(973)
        for size in [0,1,55,56,63,64,65,127,128,255,1024,65536]:
            data=rng.randbytes(size);digest=C.create_string_buffer(32)
            lib.sha256(data,size,digest);self.assertEqual(digest.raw,hashlib.sha256(data).digest())
            self.assertEqual(lib.crc32(data,size),zlib.crc32(data))
            state=Sha();lib.sha256_init(C.byref(state))
            for start in range(0,size,13):
                part=data[start:start+13];lib.sha256_update(C.byref(state),part,len(part))
            lib.sha256_final(C.byref(state),digest);self.assertEqual(digest.raw,hashlib.sha256(data).digest())
    def test_image_fields_and_bounds(self):
        h=image();self.assertTrue(lib.image_header_valid(C.byref(h)))
        for field,value in [('magic',b'WRONG000'),('board',0),('format',2),('address',0x20004000),('size',1023),('size',131073),('build',0),('flags',1)]:
            h=image();setattr(h,field,value);self.assertFalse(lib.image_header_valid(C.byref(h)),field)
    def test_vectors_stay_in_loaded_image(self):
        h=image()
        for sp,pc,valid in [(0x20040000,0x20010401,True),(0x20040004,0x20010401,False),(0x20010000,0x20010401,False),(0x2003fffc,0x20010401,False),(0x20040000,0x20010400,False),(0x20040000,0x20004001,False),(0x20040000,0x20010801,False)]:
            self.assertEqual(lib.image_vectors_valid(C.byref(h),struct.pack('<II',sp,pc)),valid)
    def test_boot_trial_rollback_and_crc(self):
        b=Boot(active=0,pending=1);lib.boot_record_seal(C.byref(b));self.assertTrue(lib.boot_record_valid(C.byref(b)))
        self.assertEqual(lib.boot_select(C.byref(b)),1);self.assertEqual(b.tried,1)
        self.assertEqual(lib.boot_select(C.byref(b)),0);self.assertEqual(b.pending,255)
        b.generation+=1;self.assertFalse(lib.boot_record_valid(C.byref(b)))
        b.tried=1;lib.boot_record_seal(C.byref(b));self.assertFalse(lib.boot_record_valid(C.byref(b)))
    def test_homie_identifiers(self):
        for text in ['chime','entrance-chime','a','x'*48]:self.assertTrue(lib.homie_id_valid(text.encode()))
        for text in ['', '-x','x-','x--y','A','a/b','a_b','a+b','$state','x'*49]:self.assertFalse(lib.homie_id_valid(text.encode()))
    def test_form_boundaries_duplicates_and_encoding(self):
        out=C.create_string_buffer(24)
        self.assertTrue(lib.form_get(b'name=Lobby+chime+%E2%9C%93',b'name',out,len(out)));self.assertEqual(out.value.decode(),'Lobby chime ✓')
        for text in [b'name=x&name=y',b'name=%00',b'name=%0a',b'name=%xy',b'name=%',b'name='+b'x'*24,b'other=x']:
            self.assertFalse(lib.form_get(text,b'name',out,len(out)),text)
    def test_http_rejects_ambiguous_lengths_and_oversize(self):
        common=b'POST /api/config HTTP/1.1\r\nHost: chime\r\n'
        good=common+b'Content-Length: 10\r\nX-Chime-Request: 1\r\n\r\n'
        ok,r=parse_request(good);self.assertTrue(ok);self.assertEqual(r.length,10);self.assertTrue(r.custom)
        self.assertTrue(parse_request(common+b'Content-Length: 524288\r\n\r\n')[0])
        self.assertFalse(parse_request(common+b'Content-Length: 524289\r\n\r\n')[0])
        for bad in [b'Content-Length: 1\r\nContent-Length: 1\r\n',b'Content-Length: -1\r\n',b'Content-Length: 999999999999\r\n',b'Transfer-Encoding: chunked\r\n',b' Content-Length: 1\r\n',b'Authorization: a\r\nAuthorization: b\r\n']:
            self.assertFalse(parse_request(common+bad+b'\r\n')[0],bad)
        self.assertFalse(parse_request(b'GET / HTTP/1.1\r\n\r\n')[0])
    def test_basic_auth_and_password_limits(self):
        out=C.create_string_buffer(129)
        for user in [b'admin:password123',b'admin:long-password-value']:
            self.assertTrue(lib.basic_password(b'Basic '+base64.b64encode(user),out,len(out)));self.assertEqual(out.value,user[6:])
        for token in [b'Bearer abc',b'Basic %%',b'Basic '+base64.b64encode(b'root:123'),b'Basic '+base64.b64encode(b'admin:x\0y')]:self.assertFalse(lib.basic_password(token,out,len(out)))
    def test_original_wav_and_invalid_formats(self):
        data=(FW/'assets/chime.wav').read_bytes();off=C.c_uint32();length=C.c_uint32()
        self.assertTrue(lib.wav_header(data[:1024],1024,len(data),C.byref(off),C.byref(length)));self.assertEqual(off.value+length.value,len(data))
        for where,value in [(0,ord('X')),(20,3),(22,2),(34,24)]:
            damaged=bytearray(data[:1024]);damaged[where]=value;self.assertFalse(lib.wav_header(bytes(damaged),1024,len(data),C.byref(off),C.byref(length)))
        self.assertFalse(lib.wav_header(data[:1024],1024,0,C.byref(off),C.byref(length)))
    def test_description_contract_and_json_escaping(self):
        out=C.create_string_buffer(1500);n=lib.homie_description(out,len(out),'Chime "Entrada"'.encode());self.assertGreater(n,0)
        d=json.loads(out.value);self.assertEqual(d['homie'],'5.0');self.assertEqual(d['name'],'Chime "Entrada"');self.assertEqual(d['version'],6)
        led=d['nodes']['chime']['properties']['led'];self.assertEqual(led['datatype'],'boolean');self.assertTrue(led['settable']);self.assertTrue(led.get('retained',True))
        self.assertEqual(d['nodes']['chime']['properties']['led-diagnostic']['datatype'],'string')
        self.assertEqual(d['nodes']['chime']['properties']['led-command']['datatype'],'string')
        self.assertEqual(set(d['nodes']['chime']['properties']['error']['format'].split(',')),{'none','codec','wav','underrun','storage','dma-timeout','audio-timeout','dma-error','i2s-error','i2s-start'})
        play=d['nodes']['chime']['properties']['play'];self.assertTrue(play['settable']);self.assertFalse(play['retained']);self.assertEqual(play['format'],'original')
        self.assertEqual(d['nodes']['system']['properties']['ip']['datatype'],'string');self.assertIn('web',d['nodes']['system']['properties']);self.assertIn('diagnostic',d['nodes']['chime']['properties']);
        self.assertNotIn('parent',d);self.assertNotIn('root',d)
        self.assertEqual(lib.homie_description(out,10,b'Chime'),0)
    def test_ip_rendering(self):
        lib.ip_format.argtypes=[C.c_uint32,C.c_void_p]
        out=C.create_string_buffer(16)
        for ip,text in [(0xc0000201,b'192.0.2.1'),(0xc0a80401,b'192.168.4.1'),(0xffffffff,b'255.255.255.255'),(0,b'0.0.0.0')]:
            lib.ip_format(ip,out);self.assertEqual(out.value,text)
    def test_uint_overflow_and_range(self):
        out=C.c_uint32()
        for s in [b'0',b'100',b'007']:self.assertTrue(lib.parse_uint(s,100,C.byref(out)))
        for s in [b'',b'-1',b'101',b'99999999999999999999999',b'1.0',b' 1']:self.assertFalse(lib.parse_uint(s,100,C.byref(out)))
    def test_mqtt_fragmentation_and_oversize(self):
        p=b'\x30\x08\x00\x01t\x00test';header=C.c_size_t()
        for end in range(len(p)):self.assertEqual(lib.mqtt_frame_size(p,end,2048,C.byref(header)),0)
        self.assertEqual(lib.mqtt_frame_size(p,len(p),2048,C.byref(header)),len(p))
        self.assertEqual(lib.mqtt_frame_size(p,len(p),1,C.byref(header)),-1)
        self.assertEqual(lib.mqtt_frame_size(b'\x30\xff\xff\xff\xff',5,2048,C.byref(header)),-1)
        parsed=Publish();self.assertTrue(lib.mqtt_parse_publish(p,len(p),C.byref(parsed)));self.assertEqual(C.string_at(parsed.payload,parsed.payload_len),b'test')
    def test_random_frames_and_headers_no_crash(self):
        rng=random.Random(40);header=C.c_size_t();pub=Publish()
        for i in range(10000):
            data=rng.randbytes(rng.randrange(80));lib.mqtt_frame_size(data,len(data),2048,C.byref(header));lib.mqtt_parse_publish(data,len(data),C.byref(pub))
        for i in range(1000):
            parse_request(bytes(rng.randrange(1,128) for j in range(rng.randrange(80))))

if __name__=='__main__':unittest.main(verbosity=2)
