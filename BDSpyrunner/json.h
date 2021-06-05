﻿#pragma once
#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"

using namespace std;
using namespace rapidjson;

Document toJson(const char* str) {
	Document doc;
	return move(doc.Parse(str));
}

Document toJson(const string& str) {
	Document doc;
	return move(doc.Parse(str));
}

string toString(const Value& value) {
	StringBuffer s;
	PrettyWriter w(s);
	value.Accept(w);
	return s.GetString();
}

string toShortString(const Value& value) {
	StringBuffer s;
	Writer w(s);
	value.Accept(w);
	return s.GetString();
}
