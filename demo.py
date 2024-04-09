import minitelserver
import time
import threading
import os 
import socket

with open("minitel 3614.vdt", "rb") as f:
    page = f.read()

startAt = time.time()
visitors = 0

if os.path.isfile("visitors.txt"):
    with open("visitors.txt", "r") as f:
        visitors = int(f.read())

class MinitelClient:
    def __init__(self, transport):
        self.transport = transport
        self.is_alive = True
        self.buffer = bytearray()
        self.event = threading.Event()
        self.lock = threading.Lock()

    def connection_lost(self):
        print("conn_lost")
        self.is_alive = False
        self.buffer = None
        
    def byte_received(self, byte):
        with self.lock:
            self.buffer.append(byte)
            self.event.set()
        

def handle_client(client):
    global visitors
    
    nbHours = round((time.time() - startAt) / (60 * 60))
    nbDays = nbHours // 24
    nbHours %= 24
    
    welcome = b" Bienvenue sur le serveur !\r\n"
    welcome += b" En ligne depuis "
    
    if nbHours == 0 and nbDays == 0:
        welcome += b"moins d'une heure..."
    
    else:
        if nbDays >= 1:
            welcome += b"%d jour" % nbDays
            if nbDays >= 2:
                welcome += b"s"
                
            if nbHours >= 1:
                welcome += b" et "
        
        if nbHours >= 1:
            welcome += b"%d heure" % nbHours
            if nbHours >= 2:
                welcome += b"s"
        
    welcome += b"\r\n"
    welcome += b" %d connexions depuis l'ouverture !" % visitors
    
    visitors += 1
    with open("visitors.txt", "w") as f:
        f.write(str(visitors))
    
    client.transport.transmit(page.replace(b"{TEXT}", welcome))
    
    while 1:
        client.event.wait()
        with client.lock:
            client.event.clear()
            print(client.buffer)
            client.buffer.clear()
        
        
def callback(transport):
    client = MinitelClient(transport)
    thread = threading.Thread(target=handle_client, args=(client,))
    thread.daemon = True
    thread.start()
    
    return client
    
    
minitelserver.start(callback)
