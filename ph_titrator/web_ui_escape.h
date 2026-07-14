#pragma once

#include <Arduino.h>

String htmlEscape(const String &value);
String jsonEscape(const String &value);
void appendWebUiDocumentOpen(String &page);
