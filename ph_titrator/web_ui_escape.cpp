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
