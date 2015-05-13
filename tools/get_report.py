from perf import stat_pb2
from proto import remote_pb2
from google.protobuf import text_format

import socket
import struct
import sys
import zlib


def varint_encode(n):
    result = ''
    while n > 0x7f:
        result += chr(0x80 | (n & 0x7f))
        n >>= 7
    return result + chr(n & 0x7f)


def varint_decode(buf):
    result = 0
    i = 0
    while ord(buf[i]) > 0x7f:
        result += (ord(buf[i]) & 0x7f) << 7*i
        i += 1
    result += ord(buf[i]) << 7*i
    i += 1
    return result, buf[i:]


def get_message(sock, msgtype):
    data = ''
    buf = ''
    while True:
        buf = sock.recv(1024)
        if not buf: break
        data += buf
    msg_buf = zlib.decompress(data)
    packed_len, msg_buf = varint_decode(msg_buf)
                            
    msg = msgtype()
    msg.ParseFromString(msg_buf)
    return msg


def send_message(sock, message):
    s = message.SerializeToString()
    packed_len = varint_encode(len(s))
    sock.sendall(zlib.compress(packed_len + s))


def Main(args):
    s = socket.create_connection((args[0], args[1]))

    m = remote_pb2.Universal()
    r = m.Extensions[remote_pb2.StatReport.extension].metric.add()
    r.name = stat_pb2._METRIC_NAME.values_by_name[args[2]].number
    send_message(s, m)

    m = get_message(s, remote_pb2.Universal)
    print text_format.MessageToString(m)

    s.close()
    

if __name__ == '__main__':
    Main(sys.argv[1:])
