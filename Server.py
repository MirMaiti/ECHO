import asyncio
import os
import io
import wave
import logging
from typing import Optional, Dict, Any

import websockets
import asyncpg
import httpx
from openai import AsyncOpenAI
from dotenv import load_dotenv

load_dotenv()

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("edge_ai_server")

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY")
MASTER_DB_URL  = os.getenv("DATABASE_URL")
PORT           = int(os.getenv("PORT", "8000"))

client = AsyncOpenAI(api_key=OPENAI_API_KEY)
db_pool: Optional[asyncpg.Pool] = None

# ==========================================
# DATABASE ROUTING
# ==========================================
async def init_master_db():
    global db_pool
    if not MASTER_DB_URL:
        logger.warning("No DATABASE_URL. Running in schema-bypass mode.")
        return
    try:
        db_pool = await asyncpg.create_pool(MASTER_DB_URL, min_size=1, max_size=20)
    except Exception as e:
        logger.error(f"DB connection failed: {e}")

async def get_client_config_by_mac(mac_address: str) -> Dict[str, Any]:
    if db_pool:
        try:
            async with db_pool.acquire() as conn:
                record = await conn.fetchrow(
                    "SELECT mac_address, client_id, room_number, client_db_endpoint FROM device_registry WHERE mac_address = $1", 
                    mac_address
                )
                if record: return dict(record)
        except Exception as e:
            logger.error(f"DB Query error: {e}")
            
    return {
        "mac_address": mac_address,
        "client_id": "test_facility",
        "room_number": "Prototype",
        "client_db_endpoint": None
    }

async def fetch_client_database(client_config: Dict[str, Any]) -> Dict[str, Any]:
    endpoint = client_config.get('client_db_endpoint')
    if endpoint:
        try:
            async with httpx.AsyncClient(timeout=5.0) as http_client:
                response = await http_client.get(f"{endpoint}/get-context", params={"room": client_config['room_number']})
                response.raise_for_status()
                return response.json()
        except Exception as e:
            logger.error(f"Client DB fetch failed: {e}")

    return {
        "hotel_name": "Antar Luxury Suites",
        "wifi_password": "Antar-Guest",
        "persona": "You are a discreet luxury hotel concierge AI.",
        "guest_name": "Valued Guest"
    }

# ==========================================
# DYNAMIC GREETING GENERATOR
# ==========================================
async def generate_and_play_greeting(websocket: websockets.WebSocketServerProtocol, client_config: Dict[str, Any]):
    """Automatically triggered upon successful MAC authentication."""
    client_db = await fetch_client_database(client_config)
    
    greeting_text = f"Welcome to {client_db['hotel_name']}, {client_db['guest_name']}. I am connected and ready. How may I assist you today?"
    logger.info(f"[{client_config['mac_address']}] Generating Greeting: {greeting_text}")

    try:
        tts_response = await client.audio.speech.create(
            model="tts-1", voice="alloy", input=greeting_text, response_format="pcm"
        )
        response_pcm = tts_response.content
        for i in range(0, len(response_pcm), 4096):
            await websocket.send(response_pcm[i:i+4096])
            await asyncio.sleep(0.01)
    except Exception as e:
        logger.error(f"Failed to play greeting: {e}")

# ==========================================
# AI PIPELINE
# ==========================================
def pcm_to_wav_buffer(pcm_data: bytes) -> io.BytesIO:
    wav_io = io.BytesIO()
    with wave.open(wav_io, 'wb') as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(16000)
        wav_file.writeframes(pcm_data)
    wav_io.seek(0)
    wav_io.name = "audio.wav"
    return wav_io

async def process_ai_interaction(websocket: websockets.WebSocketServerProtocol, pcm_audio_data: bytes, client_config: Dict[str, Any]):
    try:
        mac_id = client_config['mac_address']
        client_db = await fetch_client_database(client_config)

        wav_file = pcm_to_wav_buffer(pcm_audio_data)
        transcript = await client.audio.transcriptions.create(model="whisper-1", file=wav_file)
        user_text = transcript.text
        if not user_text.strip(): return

        system_prompt = f"{client_db.get('persona')} Location: Room {client_config.get('room_number')} at {client_db.get('hotel_name')}. Addressing: {client_db.get('guest_name')}. Keep responses to 1-2 brief sentences."
        
        llm_response = await client.chat.completions.create(
            model="gpt-4o-mini",
            messages=[{"role": "system", "content": system_prompt}, {"role": "user", "content": user_text}]
        )
        ai_reply = llm_response.choices[0].message.content

        tts_response = await client.audio.speech.create(
            model="tts-1", voice="alloy", input=ai_reply, response_format="pcm"
        )
        response_pcm = tts_response.content
        for i in range(0, len(response_pcm), 4096):
            await websocket.send(response_pcm[i:i+4096])
            await asyncio.sleep(0.01)

    except Exception as e:
        logger.error(f"Pipeline error for {client_config.get('mac_address')}: {e}")

# ==========================================
# WEBSOCKET ROUTING
# ==========================================
async def handle_client(websocket: websockets.WebSocketServerProtocol, path: str):
    client_config = None
    is_recording = False
    audio_buffer = bytearray()

    try:
        async for message in websocket:
            if isinstance(message, str):
                if message.startswith("REGISTER_DEVICE:"):
                    mac_address = message.split(":")[1]
                    client_config = await get_client_config_by_mac(mac_address)
                    await websocket.send("AUTH_SUCCESS")
                    
                    # Spawn background task to generate and stream the database-aware greeting
                    asyncio.create_task(generate_and_play_greeting(websocket, client_config))

                elif message == "START_RECORDING":
                    is_recording = True
                    audio_buffer.clear()

                elif message == "STOP_RECORDING":
                    is_recording = False
                    if len(audio_buffer) > 0 and client_config:
                        asyncio.create_task(process_ai_interaction(websocket, bytes(audio_buffer), client_config))

            elif isinstance(message, bytes) and is_recording:
                audio_buffer.extend(message)

    except websockets.exceptions.ConnectionClosed:
        pass

async def main():
    await init_master_db()
    logger.info(f"Starting Edge AI Server on port {PORT}...")
    async with websockets.serve(handle_client, "0.0.0.0", PORT):
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
