#!/usr/bin/env python3
"""
IEC 61850 MMS 模拟器
====================
模拟 IEC 61850 网关，接收告警推送请求，
返回 MMS 确认，并记录告警历史。

监听 HTTP 端口（简化模拟），接收 POST /mms/report 请求，
解析告警内容，记录到内存数据库，返回确认。
"""

import json
import time
import argparse
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime
from collections import deque


alarm_history = deque(maxlen=10000)
stats = {"reports_received": 0, "reports_confirmed": 0, "errors": 0}


class IEC61850Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path == "/mms/report":
            content_length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(content_length) if content_length > 0 else b"{}"

            try:
                report = json.loads(body)
                alarm_record = {
                    "received_at": datetime.now().isoformat(),
                    "ied_name": report.get("ied_name", "UNKNOWN"),
                    "dataset": report.get("dataset", ""),
                    "turbine_id": report.get("turbine_id", 0),
                    "blade_id": report.get("blade_id", 0),
                    "alarm_type": report.get("alarm_type", ""),
                    "alarm_level": report.get("alarm_level", ""),
                    "message": report.get("message", ""),
                    "value": report.get("value", 0.0),
                    "threshold": report.get("threshold", 0.0),
                }
                alarm_history.append(alarm_record)
                stats["reports_received"] += 1

                confirmation = {
                    "mms_confirm": True,
                    "report_id": report.get("report_id", ""),
                    "timestamp": int(time.time() * 1000),
                    "reason_code": 0,
                    "reason_string": "Accepted",
                }
                stats["reports_confirmed"] += 1

                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps(confirmation).encode())

            except Exception as e:
                stats["errors"] += 1
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"error": str(e)}).encode())

        elif self.path == "/mms/associate":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({
                "associate_confirm": True,
                "server_status": "online",
                "supported_services": ["report", "get-data-set", "set-data-set-value"],
            }).encode())
        else:
            self.send_response(404)
            self.end_headers()

    def do_GET(self):
        if self.path == "/mms/status":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            status = {
                "iec61850_gateway": "online",
                "ied_name": "TURBINE_MONITOR",
                "uptime_s": time.time() - start_time,
                "stats": stats,
                "recent_alarms": list(alarm_history)[-20:],
            }
            self.wfile.write(json.dumps(status, indent=2, ensure_ascii=False).encode())
        elif self.path == "/mms/alarms":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(list(alarm_history), indent=2, ensure_ascii=False).encode())
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        pass


start_time = time.time()


def stats_printer():
    while True:
        time.sleep(10)
        print(f"[IEC61850 Sim] Reports: {stats['reports_received']} | "
              f"Confirmed: {stats['reports_confirmed']} | "
              f"Errors: {stats['errors']} | "
              f"History: {len(alarm_history)}")


def main():
    parser = argparse.ArgumentParser(description="IEC 61850 MMS Gateway Simulator")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8102)
    args = parser.parse_args()

    server = HTTPServer((args.host, args.port), IEC61850Handler)
    print(f"[IEC61850 Sim] Listening on {args.host}:{args.port}")
    print(f"[IEC61850 Sim] Endpoints:")
    print(f"  POST /mms/report     - Receive alarm report")
    print(f"  POST /mms/associate  - MMS association")
    print(f"  GET  /mms/status     - Gateway status + stats")
    print(f"  GET  /mms/alarms     - All alarm history")

    t = threading.Thread(target=stats_printer, daemon=True)
    t.start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[IEC61850 Sim] Shutting down")
        server.shutdown()


if __name__ == "__main__":
    main()
