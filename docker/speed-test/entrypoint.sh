#!/bin/bash
set -e

MODE=$1
shift

case "$MODE" in
    server)
        echo "Starting LSQUIC Speed Test Server..."
        echo "Listening on 0.0.0.0:9331"
        exec /app/speed_server -c speedtest,/app/cert.pem,/app/key.pem -s 0.0.0.0:9331 "$@"
        ;;
    client)
        echo "Starting LSQUIC Speed Test Client..."
        exec /app/speed_client "$@"
        ;;
    *)
        echo "Usage (with --network host):"
        echo "  Server: docker run --network host lsquic-speed-test server"
        echo "  Client: docker run --network host lsquic-speed-test client -s <server_ip>:9331 -b 1G"
        echo ""
        echo "Client options:"
        echo "  -s HOST:PORT   Server address (required)"
        echo "  -b BYTES       Bytes to send (default: 1G)"
        echo "                 Supports K, M, G suffixes"
        echo ""
        echo "Examples:"
        echo "  docker run --network host lsquic-speed-test client -s 192.168.1.100:9331 -b 500M"
        echo "  docker run --network host lsquic-speed-test client -s 10.0.0.1:9331 -b 2G"
        exit 1
        ;;
esac
