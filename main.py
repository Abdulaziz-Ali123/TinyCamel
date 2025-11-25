import requests
import re
import os

url = "http://192.168.4.1/stream"

boundary = b"--frame"
label_re = re.compile(rb"X-Label:(.*)\r\n")

print("Connecting to ESP32 stream...")

r = requests.get(url, stream=True)
raw = r.raw  # raw socket-level reader

while True:
    # 1. Read until boundary
    line = raw.readline()
    if boundary not in line:
        continue

    # 2. Read headers
    headers = b""
    while True:
        line = raw.readline()
        if line == b"\r\n":
            break
        headers += line

    # 3. Extract label and content length
    m_label = label_re.search(headers)
    if not m_label:
        continue

    label = m_label.group(1).decode().strip() + ".jpg"

    m_len = re.search(rb"Content-Length:\s*(\d+)", headers)
    if not m_len:
        continue

    length = int(m_len.group(1))

    # 4. Read EXACT JPEG bytes
    jpeg = raw.read(length)

    # 5. Save the JPEG
    os.makedirs("images", exist_ok=True)
    with open("images/"+label, "wb") as f:
        f.write(jpeg)

    print("Saved", label)
