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
        echo "  Server: docker run --network host lsquic-speed-test server [-C cubic|bbr|adaptive]"
        echo "  Client: docker run --network host lsquic-speed-test client -s <server_ip>:9331 -b 1G [-C cubic|bbr|adaptive]"
        echo ""
        echo "Options:"
        echo "  -s HOST:PORT   Server address (required for client)"
        echo "  -b BYTES       Bytes to send (default: 1G)"
        echo "                 Supports K, M, G suffixes"
        echo "  -C ALGO        Congestion control: cubic (default), bbr, adaptive"
        echo ""
        echo "Examples:"
        echo "  docker run --network host lsquic-speed-test server -C bbr"
        echo "  docker run --network host lsquic-speed-test client -s 192.168.1.100:9331 -b 500M -C bbr"
        exit 1
        ;;
esac
