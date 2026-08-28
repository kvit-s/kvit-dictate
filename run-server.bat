@echo off
rem ============================================================================================
rem  run-server.bat - start the ASR server this machine's kvit-dictate is configured against.
rem
rem  Settings -> Server points at ws://127.0.0.1:10402/, and this is what has to be listening on
rem  the other end of it. Leave this window open while you dictate; closing it stops the server,
rem  and the client will say "the server is not answering; retrying in N s" rather than fail
rem  silently.
rem
rem  What it actually does: accepts the client's audio over a WebSocket (docs/protocol.md 2.6),
rem  and forwards it to the Qwen3-ASR endpoint on strixhalo over HTTP. The recognition happens
rem  there; this process is the protocol adapter in between.
rem
rem  The three backend options are not decoration - docs/setup.md 2.2 has the long version:
rem    window_ms=30000   how much audio each request carries
rem    vad_gate=true     the endpoint answers 500 ms of silence with an invented word ("Yeah.")
rem    --chunk-ms 2000   how often a partial hypothesis is asked for
rem
rem  Bound to 127.0.0.1 on purpose. There is no authentication and ws:// is unencrypted
rem  (docs/protocol.md 2.8); listening on 0.0.0.0 would let anything on the network send audio.
rem
rem    run-server.bat              default: the Qwen3-ASR endpoint on strixhalo
rem    run-server.bat mock         the scripted fixture backend, for testing the client alone
rem ============================================================================================

setlocal

set "PY=C:\Python312\python.exe"
if not exist "%PY%" (
    echo [run-server] %PY% not found. The server needs CPython 3.12 or newer.
    exit /b 3
)

set "LISTEN=127.0.0.1:10402"
set "BASE_URL=http://strixhalo.lan:10301"

cd /d "%~dp0server"

if /I "%~1"=="mock" (
    echo [run-server] mock backend on ws://%LISTEN%/ - types scripted sentences, not your speech
    "%PY%" -m kvit_asr --listen %LISTEN% --backend mock
    exit /b %ERRORLEVEL%
)

echo [run-server] listening on ws://%LISTEN%/
echo [run-server] recognition by %BASE_URL%
echo [run-server] leave this window open; Ctrl+C stops it
echo.

"%PY%" -m kvit_asr ^
    --listen %LISTEN% ^
    --backend openai-http ^
    --model Qwen/Qwen3-ASR-1.7B ^
    --backend-opt base_url=%BASE_URL% ^
    --backend-opt window_ms=30000 ^
    --backend-opt vad_gate=true ^
    --chunk-ms 2000 ^
    --endpoint-silence-ms 1500

exit /b %ERRORLEVEL%
