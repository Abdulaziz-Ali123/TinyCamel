import requests
import re

url = "http://192.168.4.1/stream"   # ESP32 AP default

# Boundary
boundary = b"--frame"

# Regex for label header
label_re = re.compile(rb"X-Label:(.*)\r\n")

print("Connecting to ESP32 stream...")

r = requests.get(url, stream=True)

buffer = b""

for chunk in r.iter_content(chunk_size=1024):
    buffer += chunk

    # Look for frame boundary
    if boundary in buffer:
        parts = buffer.split(boundary)
        frame_part = parts[-1]  # newest

        # Extract label header
        header_end = frame_part.find(b"\r\n\r\n")
        header = frame_part[:header_end]

        match = label_re.search(header)
        if not match:
            continue

        label = match.group(1).decode().strip()
        filename = label

        # Extract JPEG
        jpeg = frame_part[header_end+4:]

        # Truncate if boundary appears inside jpeg
        next_bound = jpeg.find(boundary)
        if next_bound != -1:
            jpeg = jpeg[:next_bound]

        # Save file
        with open(filename, "wb") as f:
            f.write(jpeg)

        print("Saved", filename)

        # Reset buffer
        buffer = b""