#include "json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {
class Reader {
    const std::string &text;
    size_t cursor=0;
    [[noreturn]] void Fail() const { throw std::runtime_error("Invalid settings JSON at byte "+std::to_string(cursor)); }
    void Space() { while(cursor<text.size() && (text[cursor]==' ' || text[cursor]=='\n' || text[cursor]=='\r' || text[cursor]=='\t')) ++cursor; }
    bool Eat(char c) { Space(); if(cursor<text.size() && text[cursor]==c) { ++cursor; return true; } return false; }
    unsigned Hex() {
        unsigned value=0;
        for(int i=0;i<4;++i) {
            if(cursor==text.size()) Fail();
            char c=text[cursor++]; unsigned digit=c>='0' && c<='9' ? unsigned(c-'0') :
                c>='a' && c<='f' ? unsigned(c-'a'+10) : c>='A' && c<='F' ? unsigned(c-'A'+10) : 16;
            if(digit==16) Fail(); value=value*16+digit;
        }
        return value;
    }
    void Utf8(std::string &s, unsigned c) {
        if(c<128) s+=char(c);
        else if(c<2048) { s+=char(0xc0|(c>>6)); s+=char(0x80|(c&63)); }
        else if(c<65536) { s+=char(0xe0|(c>>12)); s+=char(0x80|((c>>6)&63)); s+=char(0x80|(c&63)); }
        else { s+=char(0xf0|(c>>18)); s+=char(0x80|((c>>12)&63)); s+=char(0x80|((c>>6)&63)); s+=char(0x80|(c&63)); }
    }
    std::string String() {
        if(!Eat('"')) Fail(); std::string s;
        while(cursor<text.size()) {
            unsigned char c=static_cast<unsigned char>(text[cursor++]);
            if(c=='"') return s;
            if(c<32) Fail();
            if(c!='\\') { s+=char(c); continue; }
            if(cursor==text.size()) Fail();
            switch(text[cursor++]) {
            case '"': s+='"'; break; case '\\': s+='\\'; break; case '/': s+='/'; break;
            case 'b': s+='\b'; break; case 'f': s+='\f'; break; case 'n': s+='\n'; break;
            case 'r': s+='\r'; break; case 't': s+='\t'; break;
            case 'u': {
                unsigned code=Hex();
                if(code>=0xd800 && code<=0xdbff) {
                    if(cursor+2>text.size() || text[cursor++]!='\\' || text[cursor++]!='u') Fail();
                    unsigned low=Hex(); if(low<0xdc00 || low>0xdfff) Fail();
                    code=0x10000+((code-0xd800)<<10)+(low-0xdc00);
                } else if(code>=0xdc00 && code<=0xdfff) Fail();
                Utf8(s,code); break;
            }
            default: Fail();
            }
        }
        Fail();
    }
    Json Value(unsigned depth) {
        if(depth>32) Fail(); Space(); if(cursor==text.size()) Fail(); Json v;
        if(text[cursor]=='"') { v.kind=Json::Kind::String; v.string=String(); }
        else if(Eat('{')) {
            v.kind=Json::Kind::Object;
            if(!Eat('}')) do {
                auto key=String(); if(!Eat(':')) Fail();
                if(!v.object.emplace(key,Value(depth+1)).second) Fail();
                if(Eat('}')) return v;
                if(!Eat(',')) Fail();
            } while(true);
        } else if(Eat('[')) {
            v.kind=Json::Kind::Array;
            if(!Eat(']')) do {
                v.array.push_back(Value(depth+1));
                if(Eat(']')) return v;
                if(!Eat(',')) Fail();
            } while(true);
        } else if(text.compare(cursor,4,"true")==0) { cursor+=4; v.kind=Json::Kind::Boolean; v.boolean=true; }
        else if(text.compare(cursor,5,"false")==0) { cursor+=5; v.kind=Json::Kind::Boolean; }
        else if(text.compare(cursor,4,"null")==0) cursor+=4;
        else {
            size_t start=cursor; if(text[cursor]=='-') ++cursor;
            auto digits=[&]() { size_t begin=cursor; while(cursor<text.size() && text[cursor]>='0' && text[cursor]<='9') ++cursor; if(begin==cursor) Fail(); };
            if(cursor<text.size() && text[cursor]=='0') ++cursor; else digits();
            if(cursor<text.size() && text[cursor]=='.') { ++cursor; digits(); }
            if(cursor<text.size() && (text[cursor]=='e' || text[cursor]=='E')) {
                ++cursor; if(cursor<text.size() && (text[cursor]=='+' || text[cursor]=='-')) ++cursor; digits();
            }
            v.kind=Json::Kind::Number; v.number=std::stod(text.substr(start,cursor-start));
            if(!std::isfinite(v.number)) Fail();
        }
        return v;
    }
public:
    explicit Reader(const std::string &s):text(s) {}
    Json Read() { auto v=Value(0); Space(); if(cursor!=text.size()) Fail(); return v; }
};
}
const Json &Json::At(const std::string &key) const
{
    if(kind!=Kind::Object || !object.contains(key)) throw std::runtime_error("Missing settings field: "+key);
    return object.at(key);
}
Json ReadJson(const std::string &text) { return Reader(text).Read(); }
std::string JsonString(const std::string &text)
{
    std::string result="\"";
    for(unsigned char c:text) {
        if(c=='"' || c=='\\') { result+='\\'; result+=char(c); }
        else if(c<32) { char escape[7]; std::snprintf(escape,sizeof(escape),"\\u%04x",unsigned(c)); result+=escape; }
        else result+=char(c);
    }
    return result+'"';
}
