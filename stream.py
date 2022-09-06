from pydoc import cli
import socket
from traceback import print_exc
import cv2
import numpy as np
from time import sleep
import json

addr = ('0.0.0.0', 8090 )
s = socket.create_server(addr)
s.listen(0)                 
buffer = []
while True:
    try:
        client, addr = s.accept()
        while True:
            content = client.recv(17)
            if len(content) == 0:
                break
            try:
                dim = json.loads(content.decode())
                l = int(dim["w"] * dim["h"])
                for _ in range(l):
                    buffer.append(int.from_bytes(client.recv(1), 'big'))
                img = np.array(buffer, dtype=np.uint8).reshape(-1, 240)
                img = cv2.cvtColor(img, cv2.COLOR_GRAY2RGB)
                # cv2.imwrite("imgs/img.png", img)
                cv2.imshow("img", img)
                if cv2.waitKey(1) == ord('q'):
                    break
                buffer = []
            except Exception as e:
                print_exc(e)
        print("Closing connection")
        client.close()
    except KeyboardInterrupt:
        break
    except Exception as e:
        print_exc(e)