#include "web_ui_escape.h"

String htmlEscape(const String &value) {
  String out = value;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

String jsonEscape(const String &value) {
  String out = value;
  out.replace("\\", "\\\\");
  out.replace("\"", "\\\"");
  out.replace("\b", "\\b");
  out.replace("\f", "\\f");
  out.replace("\n", "\\n");
  out.replace("\r", "\\r");
  out.replace("\t", "\\t");
  return out;
}

void appendWebUiDocumentOpen(String &page) {
  page += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>K10 pH Titrator</title><style>");
  page += F(":root{--bg:#071014;--panel:#0d1d24;--panel2:#102a34;--line:#244c59;--text:#eaf7ff;--muted:#8db0bd;--ok:#67f09a;--warn:#ffd15c;--bad:#ff6b6b;--blue:#7bd5ff}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 20% 0,#15333b 0,#071014 38%,#04080a 100%);color:var(--text);font-family:Verdana,Geneva,sans-serif}main{max-width:880px;margin:auto;padding:16px}.top{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;margin-bottom:8px}.identity{min-width:0}.language{min-width:116px;flex:0 0 116px}.brand{letter-spacing:.08em;color:var(--muted);font-size:12px}.title{font-size:28px;font-weight:700;margin:2px 0 0;white-space:nowrap}.pill{display:flex;flex-wrap:wrap;gap:6px;justify-content:flex-end;max-width:100%;color:var(--blue)}.networkBar{justify-content:flex-start;margin:0 0 12px}.pill span{border:1px solid var(--line);border-radius:999px;padding:7px 10px;background:#071820;white-space:nowrap}");
  page += F(".hero{border:1px solid var(--line);border-radius:10px;background:linear-gradient(135deg,#0f2630,#081219);padding:18px;margin-bottom:12px;display:grid;grid-template-columns:1.2fr .8fr;gap:14px}.ph{font-size:72px;line-height:.95;font-weight:800}.unit{font-size:18px;color:var(--muted);margin-left:6px}.sub{color:var(--muted);margin-top:10px}.status{display:grid;gap:8px;align-content:center}.status b{font-size:22px}.grid{display:grid;grid-template-columns:repeat(6,1fr);gap:10px}.card{border:1px solid var(--line);border-radius:8px;padding:13px;background:rgba(13,29,36,.9)}.k{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.07em}.v{font-size:28px;font-weight:700;margin-top:5px}.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}");
  page += F(".bar{height:10px;background:#071014;border:1px solid var(--line);border-radius:99px;overflow:hidden;margin-top:10px}.fill{height:100%;background:linear-gradient(90deg,var(--ok),var(--warn))}.split{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}.split>.full{grid-column:1/-1}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:end}label{display:grid;gap:5px;color:var(--muted);font-size:12px;min-width:130px;flex:1}button,.btn,input,select{font:inherit;border-radius:7px;border:1px solid #3a6472;background:#0a1a21;color:var(--text);padding:10px 12px;text-decoration:none}button,.btn{display:inline-block;cursor:pointer;font-weight:700}.primary{background:#123b2b;border-color:#2d8a5a;color:#bfffd4}.danger{background:#351216;border-color:#8c3640;color:#ffd1d1}.ghost{color:var(--blue)}h2{margin:0 0 10px;font-size:16px}.tiny{font-size:12px;color:var(--muted)}.tabs{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}.tab{background:#071820;color:var(--blue)}.tab.active{background:#123b2b;border-color:#2d8a5a;color:#bfffd4}.panel{display:none}.panel.active{display:block}.full{margin-top:10px}.mini{max-width:170px}.chart{width:100%;height:260px;border:1px solid var(--line);border-radius:8px;background:#071014;cursor:crosshair}.chartbar{display:flex;gap:8px;flex-wrap:wrap;align-items:end;margin-bottom:10px}.chartbar label{min-width:110px;max-width:180px}.eqp{color:var(--warn);font-weight:700}.guide{display:grid;grid-template-columns:1fr 1fr;gap:10px}.guide p{margin:7px 0}.term{color:var(--blue);font-weight:700}@media(max-width:720px){main{padding:12px}.hero,.split,.guide{grid-template-columns:1fr}.grid{grid-template-columns:repeat(2,1fr)}.ph{font-size:58px}.top{align-items:center}.title{font-size:24px}.networkBar{margin-bottom:10px}.pill{justify-content:flex-start}.pill span{border-radius:7px;padding:6px 8px}.tabs{gap:6px}.tab{flex:1 1 29%;padding:9px 6px}.chartbar button{flex:1 1 auto}}</style></head><body><main><style>.ph,.v,#status,#mv{font-variant-numeric:tabular-nums}.ph{min-height:72px}.sub{min-height:22px}</style>");
}
