import serial
import numpy as np
from faster_whisper import WhisperModel

SERIAL_PORT = "COM5" 
BAUD_RATE = 921600 

print("Loading Whisper AI Model (Int8 / CPU)...")
model = WhisperModel("tiny.en", device="cpu", compute_type="int8")
print("Model Loaded!")

try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=4.0)
    ser.set_buffer_size(rx_size=131072)
except Exception as e:
    print(f"Error opening port: {e}\n(Make sure the Arduino Serial Monitor is CLOSED!)")
    exit()

print(f"Listening to {SERIAL_PORT} for ESP32 microphone data...")

while True:
    try:
        line = ser.readline()
        
        if b"START_AUDIO" in line:
            print("\n[!] ESP32 detected speech! Downloading 3-second audio buffer...")
            
            raw_data = bytearray()
            
            while len(raw_data) < 96000:
                chunk = ser.read(96000 - len(raw_data))
                if not chunk:
                    break 
                raw_data.extend(chunk)
            
            if len(raw_data) == 96000:
                print("Buffer received. Transcribing...")
                
                audio_array = np.frombuffer(raw_data, dtype=np.int16).astype(np.float32) / 32768.0
                
                # THE MAGIC FIX: We tell Whisper what words to expect before it translates
                segments, _ = model.transcribe(
                    audio_array, 
                    beam_size=1, 
                    vad_filter=True,
                    initial_prompt="Show the room temperature details." 
                )
                
                text = " ".join([segment.text for segment in segments]).lower().strip()
                
                print(f"Heard: '{text}'")
                
                if text == "":
                    print("Ignored (Silence/Static filtered out).")
                # We also added "aperture", "boom", and "better" to act as a fail-safe net
                elif any(word in text for word in ["room", "temperature", "details", "aperture", "better", "boom"]):
                    print(">>> COMMAND MATCH! Sending 'R' to ESP32 screen... <<<")
                    ser.write(b'R')
                else:
                    print("No matching command. Waiting...")
            else:
                print(f"Buffer under-run. Expected 96000 bytes, got {len(raw_data)}.")
                
    except Exception as e:
        print(f"Connection lost: {e}")
        break