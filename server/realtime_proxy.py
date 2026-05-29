import asyncio
import os
import json
import logging
import websockets
from dotenv import load_dotenv

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

# Load environment variables
load_dotenv()

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY")
if not OPENAI_API_KEY:
    logger.error("OPENAI_API_KEY not found in environment variables.")
    exit(1)

# OpenAI Realtime API Endpoint
OPENAI_MODEL = os.getenv("OPENAI_MODEL", "gpt-4o-realtime-preview")
OPENAI_WS_URL = f"wss://api.openai.com/v1/realtime?model={OPENAI_MODEL}"

async def proxy_handler(client_ws):
    """
    Handles a connection from the ESP32 client, connects to OpenAI,
    and relays messages in both directions.
    """
    client_ip = client_ws.remote_address[0]
    logger.info(f"New connection from ESP32 at {client_ip}")

    headers = {
        "Authorization": f"Bearer {OPENAI_API_KEY}"
    }

    try:
        # Connect to OpenAI Realtime API
        logger.info("Connecting to OpenAI Realtime API...")
        async with websockets.connect(OPENAI_WS_URL, additional_headers=headers) as openai_ws:
            logger.info("Connected to OpenAI successfully!")

            # Task to forward messages from ESP32 -> OpenAI
            async def client_to_openai():
                try:
                    import base64
                    async for message in client_ws:
                        if isinstance(message, bytes):
                            # It's binary audio data from the ESP32
                            logger.info(f"ESP32 -> OpenAI: Audio chunk ({len(message)} bytes)")
                            b64_audio = base64.b64encode(message).decode('utf-8')
                            payload = {
                                "type": "input_audio_buffer.append",
                                "audio": b64_audio
                            }
                            await openai_ws.send(json.dumps(payload))
                        else:
                            # It's a text message (like session.update)
                            logger.info(f"ESP32 -> OpenAI: {message[:100]}...")
                            await openai_ws.send(message)
                except websockets.exceptions.ConnectionClosed:
                    logger.info("ESP32 disconnected.")
                except Exception as e:
                    logger.error(f"Error in client_to_openai: {e}")

            # Task to forward messages from OpenAI -> ESP32
            async def openai_to_client():
                try:
                    import base64
                    import json
                    async for message in openai_ws:
                        try:
                            data = json.loads(message)
                            if data.get("type") in ("response.output_audio.delta", "response.audio.delta") and "delta" in data:
                                # Decode base64 audio and send as binary to ESP32
                                b64_audio = data["delta"]
                                raw_audio = base64.b64decode(b64_audio)
                                await client_ws.send(raw_audio)
                                logger.info(f"OpenAI -> ESP32: Audio chunk ({len(raw_audio)} bytes)")
                            else:
                                # Send other JSON events as text
                                logger.info(f"OpenAI -> ESP32: {message[:100]}...")
                                await client_ws.send(message)
                        except json.JSONDecodeError:
                            # Not JSON, forward as is
                            await client_ws.send(message)
                except websockets.exceptions.ConnectionClosed:
                    logger.info("OpenAI disconnected.")
                except Exception as e:
                    logger.error(f"Error in openai_to_client: {e}")

            # Run both forwarding tasks concurrently
            await asyncio.gather(
                client_to_openai(),
                openai_to_client()
            )

    except Exception as e:
        logger.error(f"Failed to connect or relay to OpenAI: {e}")
    finally:
        logger.info(f"Connection closed for {client_ip}")

async def main():
    host = "0.0.0.0"
    port = 8080
    
    logger.info(f"Starting OpenAI Realtime WebSocket Proxy on ws://{host}:{port}")
    logger.info("ESP32 should connect without SSL/TLS (ws:// instead of wss://)")
    
    async with websockets.serve(proxy_handler, host, port):
        await asyncio.Future()  # run forever

if __name__ == "__main__":
    asyncio.run(main())
