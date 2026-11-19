// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/wire-list-response.hpp>

#include <algorithm>
#include <cstring>

namespace dtv::internal {
namespace {

bool utf8(const std::uint8_t *s, std::size_t n)
{
    for (std::size_t i = 0; i < n;) {
        auto c = s[i];
        if (!c) return false;
        if (c < 0x80) { ++i; continue; }
        std::size_t k = 0; std::uint8_t lo = 0x80, hi = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) k = 2;
        else if (c >= 0xe0 && c <= 0xef) { k = 3; if (c == 0xe0) lo = 0xa0; if (c == 0xed) hi = 0x9f; }
        else if (c >= 0xf0 && c <= 0xf4) { k = 4; if (c == 0xf0) lo = 0x90; if (c == 0xf4) hi = 0x8f; }
        else return false;
        if (k > n - i || s[i + 1] < lo || s[i + 1] > hi) return false;
        for (std::size_t j = 2; j < k; ++j) if (s[i + j] < 0x80 || s[i + j] > 0xbf) return false;
        i += k;
    }
    return true;
}

struct Writer {
    std::uint8_t *p; std::size_t n = 0;
    void u8(std::uint8_t v) { if (p) p[n] = v; ++n; }
    void u16(std::uint16_t v) { u8(v); u8(v >> 8); }
    void u32(std::uint32_t v) { for (int i=0;i<4;++i) u8(v >> (8*i)); }
    void u64(std::uint64_t v) { for (int i=0;i<8;++i) u8(v >> (8*i)); }
    template<std::size_t N> void text(const WireText<N>&v) { u16(v.length); if (p && v.length) std::memcpy(p+n,v.bytes.data(),v.length); n+=v.length; }
    template<class T, class F> void opt(const WireOptional<T>&v,F f) { u8(v.present?1:0); if(v.present) f(v.value); }
};
struct Reader {
    dtv_bytes b; std::size_t n=0; bool ok=true;
    std::uint8_t u8() { if(n>=b.size){ok=false;return 0;} return b.data[n++]; }
    std::uint16_t u16() { auto a=u8(); auto c=u8(); return static_cast<std::uint16_t>(a | (c<<8)); }
    std::uint32_t u32() { std::uint32_t v=0;for(int i=0;i<4;++i)v|=static_cast<std::uint32_t>(u8())<<(8*i);return v; }
    std::uint64_t u64() { std::uint64_t v=0;for(int i=0;i<8;++i)v|=static_cast<std::uint64_t>(u8())<<(8*i);return v; }
    template<std::size_t N> bool text(WireText<N>*v) { auto z=u16(); if(!ok||z>N||z>b.size-n){ok=false;return false;} if(!utf8(b.data+n,z)){ok=false;return false;} std::copy_n(b.data+n,z,v->bytes.data());v->length=z;n+=z;return true; }
    template<class T, class F> bool opt(WireOptional<T>*v,F f) { auto tag=u8();if(!ok||tag>1){ok=false;return false;}v->present=tag==1;if(v->present)f(&v->value);return ok; }
};

template<class W> void model(W&w,const ListModel&v){w.text(v.name);}
template<class W> void error(W&w,const ListErrorRecord&v){w.u32(v.code);w.text(v.operation);w.text(v.detail);}
template<class W> void location(W&w,const ListLocation&v){w.opt(v.usb_path,[&](auto&x){w.text(x);});w.opt(v.bus,[&](auto x){w.u8(x);});w.opt(v.address,[&](auto x){w.u8(x);});}
template<class W> void function(W&w,const ListUsbFunction&v){w.u16(v.id);w.u16(v.vid);w.u16(v.pid);w.u8(v.mode);location(w,v.location);w.u8(v.serial_state);w.opt(v.serial,[&](auto&x){w.text(x);});w.opt(v.model,[&](auto&x){model(w,x);});w.u8(v.identification_reason);w.u8(v.present?1:0);w.opt(v.last_error,[&](auto&x){error(w,x);});}
template<class W> void binding(W&w,const ListBinding&v){w.u16(v.function_id);w.u16(v.local_id);}
template<class W> void caps(W&w,const ListCapabilities&v){w.u8(v.ts_scope);w.opt(v.max_pids,[&](auto x){w.u16(x);});w.u8(v.controls);if(v.controls&1){w.u16(v.lna_defaults.count);for(std::size_t i=0;i<v.lna_defaults.count;++i){const auto&x=v.lna_defaults.values[i];w.u8(x.system);w.opt(x.enabled,[&](auto y){w.u8(y?1:0);});}}if(v.controls&2){w.u16(v.lnb_values.count);for(std::size_t i=0;i<v.lnb_values.count;++i)w.u8(v.lnb_values.values[i].value);}w.u8(v.measurement_mask);}
template<class W> void receiver(W&w,const ListReceiver&v){w.u16(v.id);w.u8(v.systems);w.opt(v.binding,[&](auto&x){binding(w,x);});caps(w,v.capabilities);}
template<class W> void reader(W&w,const ListReader&v){w.u16(v.id);w.opt(v.binding,[&](auto&x){binding(w,x);});}
template<class W> void enclosure(W&w,const ListEnclosure&v){w.u16(v.id);w.opt(v.model,[&](auto&x){model(w,x);});w.u8(v.identity.serial_state);w.opt(v.identity.serial,[&](auto&x){w.text(x);});w.u8(v.identity.uniqueness);w.opt(v.limits.max_concurrent_streams,[&](auto x){w.u16(x);});}

bool text_ok(const WireText<32>&v){return v.length<=32&&utf8(v.bytes.data(),v.length);}
template<std::size_t N> bool text_ok_n(const WireText<N>&v){return v.length<=N&&utf8(v.bytes.data(),v.length);}
bool bind_ok(const WireOptional<ListBinding>&b,const WireArray<ListUsbFunction,8>&fs){if(!b.present)return true;for(std::size_t i=0;i<fs.count;++i)if(fs.values[i].id==b.value.function_id)return true;return false;}
template<class A> bool unique_ids(const A&a){for(std::size_t i=0;i<a.count;++i)for(std::size_t j=i+1;j<a.count;++j)if(a.values[i].id==a.values[j].id)return false;return true;}

WireFrameCode validate_local(const ListResponse&v)
{
    if(v.functions.count>8 || (v.receivers.present&&v.receivers.value.count>32) || (v.readers.present&&v.readers.value.count>8))return WireFrameCode::invalid_argument;
    if(v.topology.state>2 || (v.topology.expected_function_count.present&&(v.topology.expected_function_count.value==0||v.topology.expected_function_count.value>8)))return WireFrameCode::invalid_argument;
    if(!text_ok_n(v.producer.product)||!text_ok_n(v.producer.version)||(v.producer.core_version.present&&!text_ok_n(v.producer.core_version.value)))return WireFrameCode::invalid_argument;
    if(v.enclosure.present){const auto&e=v.enclosure.value;if(e.identity.serial_state>2||e.identity.uniqueness>3||(e.identity.serial_state==1)!=e.identity.serial.present||(e.identity.serial.present&&(!text_ok_n(e.identity.serial.value)||e.identity.serial.value.length==0))||(e.identity.serial_state!=1&&e.identity.serial.present))return WireFrameCode::invalid_argument;if(e.model.present&&!text_ok_n(e.model.value.name))return WireFrameCode::invalid_argument;if(e.limits.max_concurrent_streams.present&&(e.limits.max_concurrent_streams.value==0||e.limits.max_concurrent_streams.value>32))return WireFrameCode::invalid_argument;}
    for(std::size_t i=0;i<v.functions.count;++i){const auto&f=v.functions.values[i];if(f.mode>2||f.serial_state>4||f.identification_reason>3||(!f.model.present&&f.identification_reason==0)||(f.serial_state==1)!=f.serial.present||(f.serial.present&&(!text_ok_n(f.serial.value)||(f.serial_state==1&&f.serial.value.length==0)))||(f.serial_state!=1&&f.serial.present))return WireFrameCode::invalid_argument;if(f.location.usb_path.present&&!text_ok_n(f.location.usb_path.value))return WireFrameCode::invalid_argument;if(f.model.present&&!text_ok_n(f.model.value.name))return WireFrameCode::invalid_argument;if(f.last_error.present&&(f.last_error.value.code==0||!text_ok(f.last_error.value.operation)||!text_ok_n(f.last_error.value.detail)))return WireFrameCode::invalid_argument;}
    if(v.receivers.present)for(std::size_t i=0;i<v.receivers.value.count;++i){const auto&r=v.receivers.value.values[i];const auto&c=r.capabilities;if(!r.systems||(r.systems&~3)||c.ts_scope>1||c.controls&~3||c.measurement_mask&~7||(c.max_pids.present&&(c.max_pids.value==0||c.max_pids.value>8192)))return WireFrameCode::invalid_argument;if((c.controls&1)?(c.lna_defaults.count==0||c.lna_defaults.count>2):c.lna_defaults.count!=0)return WireFrameCode::invalid_argument;if((c.controls&2)?(c.lnb_values.count==0||c.lnb_values.count>2):c.lnb_values.count!=0)return WireFrameCode::invalid_argument;std::uint8_t systems=0;for(std::size_t j=0;j<c.lna_defaults.count;++j){auto x=c.lna_defaults.values[j].system;if((x!=1&&x!=2)||(systems&x)||(j&&c.lna_defaults.values[j-1].system>=x))return WireFrameCode::invalid_argument;systems|=x;}for(std::size_t j=0;j<c.lnb_values.count;++j)if((c.lnb_values.values[j].value!=0&&c.lnb_values.values[j].value!=15)||(j&&c.lnb_values.values[j-1].value>=c.lnb_values.values[j].value))return WireFrameCode::invalid_argument;}
    return WireFrameCode::ok;
}

}

WireFrameCode validate_list_response(const ListResponse&v)
{
    auto code=validate_local(v);if(code!=WireFrameCode::ok)return code;
    if(!unique_ids(v.functions)||(v.receivers.present&&!unique_ids(v.receivers.value))||(v.readers.present&&!unique_ids(v.readers.value)))return WireFrameCode::invalid_argument;
    if(v.receivers.present)for(std::size_t i=0;i<v.receivers.value.count;++i){const auto&r=v.receivers.value.values[i];if(!bind_ok(r.binding,v.functions))return WireFrameCode::invalid_argument;std::uint8_t systems=0;for(std::size_t j=0;j<r.capabilities.lna_defaults.count;++j)systems|=r.capabilities.lna_defaults.values[j].system;if((r.capabilities.controls&1)&&systems!=r.systems)return WireFrameCode::invalid_argument;}
    if(v.readers.present)for(std::size_t i=0;i<v.readers.value.count;++i)if(!bind_ok(v.readers.value.values[i].binding,v.functions))return WireFrameCode::invalid_argument;
    if(v.topology.state==0){if(v.receivers.present||v.readers.present)return WireFrameCode::invalid_argument;}
    else if(!v.receivers.present||!v.readers.present)return WireFrameCode::invalid_argument;
    if(v.enclosure.present&&v.enclosure.value.limits.max_concurrent_streams.present&&v.receivers.present&&v.enclosure.value.limits.max_concurrent_streams.value>v.receivers.value.count)return WireFrameCode::invalid_argument;
    return WireFrameCode::ok;
}

WireFrameCode list_response_payload_size(const ListResponse&v,std::size_t*size)
{
    if(!size)return WireFrameCode::invalid_argument;auto valid=validate_list_response(v);if(valid!=WireFrameCode::ok)return valid;Writer w{nullptr};
    w.n+=16;w.text(v.producer.product);w.text(v.producer.version);w.opt(v.producer.core_version,[&](auto&x){w.text(x);});w.u64(0);w.u64(0);w.u8(v.topology.state);w.opt(v.topology.expected_function_count,[&](auto x){w.u16(x);});w.opt(v.enclosure,[&](auto&x){enclosure(w,x);});w.u16(v.functions.count);for(std::size_t i=0;i<v.functions.count;++i)function(w,v.functions.values[i]);w.opt(v.receivers,[&](auto&a){w.u16(a.count);for(std::size_t i=0;i<a.count;++i)receiver(w,a.values[i]);});w.opt(v.readers,[&](auto&a){w.u16(a.count);for(std::size_t i=0;i<a.count;++i)reader(w,a.values[i]);});if(w.n>kWireMaxControlPayload)return WireFrameCode::invalid_argument;*size=w.n;return WireFrameCode::ok;
}

WireFrameCode encode_list_response(const ListResponse&v,dtv_buffer*out)
{
    if(!out||(out->capacity&&!out->data))return WireFrameCode::invalid_argument;std::size_t n=0;auto c=list_response_payload_size(v,&n);if(c!=WireFrameCode::ok)return c;if(out->capacity<n)return WireFrameCode::buffer_too_small;Writer w{out->data};w.n=0;std::copy(v.epoch.begin(),v.epoch.end(),out->data);w.n=16;w.text(v.producer.product);w.text(v.producer.version);w.opt(v.producer.core_version,[&](auto&x){w.text(x);});w.u64(v.snapshot.sequence);w.u64(v.snapshot.observed_at_ns);w.u8(v.topology.state);w.opt(v.topology.expected_function_count,[&](auto x){w.u16(x);});w.opt(v.enclosure,[&](auto&x){enclosure(w,x);});w.u16(v.functions.count);for(std::size_t i=0;i<v.functions.count;++i)function(w,v.functions.values[i]);w.opt(v.receivers,[&](auto&a){w.u16(a.count);for(std::size_t i=0;i<a.count;++i)receiver(w,a.values[i]);});w.opt(v.readers,[&](auto&a){w.u16(a.count);for(std::size_t i=0;i<a.count;++i)reader(w,a.values[i]);});out->size=n;return WireFrameCode::ok;
}

WireFrameCode decode_list_response(dtv_bytes payload,ListResponse*value)
{
    if(!value||(payload.size&&!payload.data))return WireFrameCode::invalid_argument;if(payload.size<44||payload.size>kWireMaxControlPayload)return WireFrameCode::protocol_error;Reader r{payload};ListResponse v;for(auto&x:v.epoch)x=r.u8();r.text(&v.producer.product);r.text(&v.producer.version);r.opt(&v.producer.core_version,[&](auto*x){r.text(x);});v.snapshot.sequence=r.u64();v.snapshot.observed_at_ns=r.u64();v.topology.state=r.u8();r.opt(&v.topology.expected_function_count,[&](auto*x){*x=r.u16();});r.opt(&v.enclosure,[&](auto*x){x->id=r.u16();r.opt(&x->model,[&](auto*m){r.text(&m->name);});x->identity.serial_state=r.u8();r.opt(&x->identity.serial,[&](auto*s){r.text(s);});x->identity.uniqueness=r.u8();r.opt(&x->limits.max_concurrent_streams,[&](auto*y){*y=r.u16();});});v.functions.count=r.u16();if(v.functions.count>8)return WireFrameCode::protocol_error;for(std::size_t i=0;i<v.functions.count;++i){auto&f=v.functions.values[i];f.id=r.u16();f.vid=r.u16();f.pid=r.u16();f.mode=r.u8();r.opt(&f.location.usb_path,[&](auto*x){r.text(x);});r.opt(&f.location.bus,[&](auto*x){*x=r.u8();});r.opt(&f.location.address,[&](auto*x){*x=r.u8();});f.serial_state=r.u8();r.opt(&f.serial,[&](auto*x){r.text(x);});r.opt(&f.model,[&](auto*x){r.text(&x->name);});f.identification_reason=r.u8();auto p=r.u8();if(p>1)r.ok=false;f.present=p==1;r.opt(&f.last_error,[&](auto*x){x->code=r.u32();r.text(&x->operation);r.text(&x->detail);});}
    r.opt(&v.receivers,[&](auto*a){a->count=r.u16();if(a->count>32){r.ok=false;return;}for(std::size_t i=0;i<a->count;++i){auto&x=a->values[i];x.id=r.u16();x.systems=r.u8();r.opt(&x.binding,[&](auto*b){b->function_id=r.u16();b->local_id=r.u16();});auto&c=x.capabilities;c.ts_scope=r.u8();r.opt(&c.max_pids,[&](auto*y){*y=r.u16();});c.controls=r.u8();if(c.controls&1){c.lna_defaults.count=r.u16();if(c.lna_defaults.count>2){r.ok=false;return;}for(std::size_t j=0;j<c.lna_defaults.count;++j){auto&d=c.lna_defaults.values[j];d.system=r.u8();r.opt(&d.enabled,[&](auto*z){auto b=r.u8();if(b>1)r.ok=false;*z=b==1;});}}if(c.controls&2){c.lnb_values.count=r.u16();if(c.lnb_values.count>2){r.ok=false;return;}for(std::size_t j=0;j<c.lnb_values.count;++j)c.lnb_values.values[j].value=r.u8();}c.measurement_mask=r.u8();}});
    r.opt(&v.readers,[&](auto*a){a->count=r.u16();if(a->count>8){r.ok=false;return;}for(std::size_t i=0;i<a->count;++i){auto&x=a->values[i];x.id=r.u16();r.opt(&x.binding,[&](auto*b){b->function_id=r.u16();b->local_id=r.u16();});}});
    if(!r.ok||r.n!=payload.size)return WireFrameCode::protocol_error;auto c=validate_local(v);if(c!=WireFrameCode::ok)return c;*value=v;return WireFrameCode::ok;
}

}
