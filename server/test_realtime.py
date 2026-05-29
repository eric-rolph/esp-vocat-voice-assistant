import asyncio
import websockets
import json
import os
from dotenv import load_dotenv

load_dotenv()
OPENAI_API_KEY = os.getenv("OPENAI_API_KEY")

async def test_realtime_api():
    url = "wss://api.openai.com/v1/realtime?model=gpt-realtime"
    headers = {
        "Authorization": f"Bearer {OPENAI_API_KEY}"
    }
    
    async with websockets.connect(url, additional_headers=headers) as ws:
        # Wait for session.created
        print("Waiting for session.created...")
        msg = await ws.recv()
        print("Received:", msg)
        
        # Try session.update without modalities
        payload = {
            "type": "session.update",
            "session": {
                "type": "realtime",
                "instructions": "You are a helpful assistant.",
                "output_modalities": ["audio"],
                "audio": {
                    "input": {
                        "format": {"type": "audio/pcm", "rate": 24000},
                        "turn_detection": {
                            "type": "server_vad",
                            "threshold": 0.5,
                            "prefix_padding_ms": 300,
                            "silence_duration_ms": 500
                        }
                    },
                    "output": {
                        "format": {"type": "audio/pcm", "rate": 24000},
                        "voice": "alloy"
                    }
                }
            }
        }
        print("Sending session.update...")
        await ws.send(json.dumps(payload))
        
        # Wait for response
        res = await ws.recv()
        print("Received:", res)

asyncio.run(test_realtime_api())
