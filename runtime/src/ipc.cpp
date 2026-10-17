// HIPC message parsing / building, sessions, domains and the service manager (sm:).
#include "ipc.h"

#include <mutex>
#include <unordered_map>

namespace nx {

namespace {

struct DomainState {
    std::map<uint32_t, std::shared_ptr<Service>> objects;
    uint32_t next_object = 1;
};

struct KSession : KObject {
    std::shared_ptr<Service> svc;
    bool domain = false;
    std::shared_ptr<DomainState> dom = std::make_shared<DomainState>();  // shared by clones of a domain session
};

std::unordered_map<std::string, ServiceFactory>& factories() {
    static std::unordered_map<std::string, ServiceFactory> f;
    return f;
}

std::recursive_mutex g_ipc;

constexpr uint32_t kSFCI = 0x49434653;  // 'SFCI'
constexpr uint32_t kSFCO = 0x4F434653;  // 'SFCO'

}  // namespace

void register_service(const std::string& name, ServiceFactory f) { factories()[name] = std::move(f); }

std::shared_ptr<Service> make_service(const std::string& name) {
    auto it = factories().find(name);
    if (it != factories().end()) return it->second();
    log("ipc: no implementation for service '%s', using a stub", name.c_str());
    return std::make_shared<Service>(name);
}

uint32_t Service::handle(IpcCtx& ctx) {
    auto it = cmds.find(ctx.cmd);
    if (it != cmds.end()) return it->second(ctx);
    log("ipc: %s cmd %u not implemented (stub ok)", name.c_str(), ctx.cmd);
    return 0;
}

// ---- sm: ------------------------------------------------------------------------------------------------------

namespace {

struct SmService : Service {
    SmService() : Service("sm:") {
        cmds[0] = [](IpcCtx&) { return 0u; };  // RegisterClient / Initialize
        cmds[1] = [](IpcCtx& c) {               // GetService(name)
            char name[9] = {};
            std::memcpy(name, c.in, std::min<size_t>(8, c.in_size));
            log("sm: GetService(%s)", name);
            c.push_object(make_service(name));
            return 0u;
        };
    }
};

}  // namespace

uint32_t ipc_connect(const char* port, uint64_t* out_handle) {
    std::string p = port;
    log("ConnectToNamedPort(%s)", p.c_str());
    auto s = std::make_shared<KSession>();
    s->svc = p == "sm:" ? std::make_shared<SmService>() : make_service(p);
    *out_handle = handle_add(s);
    return 0;
}

// ---- messages -------------------------------------------------------------------------------------------------

static uint32_t new_session_handle(std::shared_ptr<Service> svc) {
    auto s = std::make_shared<KSession>();
    s->svc = std::move(svc);
    return handle_add(s);
}

uint32_t ipc_request(CPU& c, uint32_t handle, uint64_t buf, uint64_t) {
    std::lock_guard lk(g_ipc);
    auto sess = std::dynamic_pointer_cast<KSession>(handle_get(handle));
    if (!sess) {
        log("ipc: request on invalid handle %x", handle);
        return 0xE401;
    }
    uint32_t* w = reinterpret_cast<uint32_t*>(buf);
    IpcCtx ctx;
    ctx.cpu = &c;
    ctx.type = w[0] & 0xFFFF;
    uint32_t nx_ = (w[0] >> 16) & 0xF, na = (w[0] >> 20) & 0xF, nb = (w[0] >> 24) & 0xF, nw = (w[0] >> 28) & 0xF;
    uint32_t ndata = w[1] & 0x3FF;
    uint32_t cflags = (w[1] >> 10) & 0xF;
    bool special = w[1] >> 31;
    size_t p = 2;
    if (special) {
        uint32_t sh = w[p++];
        bool pid = sh & 1;
        uint32_t ncopy = (sh >> 1) & 0xF, nmove = (sh >> 5) & 0xF;
        if (pid) {
            ctx.pid = w[p] | (uint64_t(w[p + 1]) << 32);
            p += 2;
        }
        for (uint32_t i = 0; i < ncopy; i++) ctx.in_copy.push_back(w[p++]);
        for (uint32_t i = 0; i < nmove; i++) ctx.in_move.push_back(w[p++]);
    }
    for (uint32_t i = 0; i < nx_; i++, p += 2) {
        uint32_t d0 = w[p], d1 = w[p + 1];
        uint64_t addr = d1 | (uint64_t((d0 >> 12) & 0xF) << 32) | (uint64_t((d0 >> 6) & 0x7) << 36);
        ctx.x.push_back({addr, d0 >> 16});
    }
    auto abw = [&](std::vector<Buffer>& v, uint32_t n) {
        for (uint32_t i = 0; i < n; i++, p += 3) {
            uint32_t size_lo = w[p], addr_lo = w[p + 1], d2 = w[p + 2];
            uint64_t addr = addr_lo | (uint64_t((d2 >> 28) & 0xF) << 32) | (uint64_t((d2 >> 2) & 0x7) << 36);
            uint64_t size = size_lo | (uint64_t((d2 >> 24) & 0xF) << 32);
            v.push_back({addr, size});
        }
    };
    abw(ctx.a, na);
    abw(ctx.b, nb);
    std::vector<Buffer> wbufs;
    abw(wbufs, nw);
    size_t data_start = p;
    // C descriptors (receive list) after the raw data.
    if (cflags >= 2) {
        size_t q = data_start + ndata;
        uint32_t nc = cflags == 2 ? 1 : cflags - 2;
        for (uint32_t i = 0; i < nc; i++, q += 2) {
            uint64_t addr = w[q] | (uint64_t(w[q + 1] & 0xFFFF) << 32);
            ctx.c.push_back({addr, w[q + 1] >> 16});
        }
    }

    if (ctx.type == 2 || ctx.type == 15) {  // Close (CMIF / TIPC)
        handle_close(handle);
        return 0;
    }
    if (ctx.type >= 16) {
        // TIPC (SDK 12+, sm: and a few system services): the command is the message type - 16, the raw data
        // follows the descriptors with no padding and no SFCI header; the response starts with the result code.
        ctx.cmd = ctx.type - 16;
        ctx.in = reinterpret_cast<const uint8_t*>(buf + data_start * 4);
        ctx.in_size = ndata * 4;
        if (g_trace) log("ipc: tipc %s cmd %u", sess->svc->name.c_str(), ctx.cmd);
        uint32_t rc = sess->svc->handle(ctx);
        for (auto& o : ctx.out_objects) ctx.out_move.push_back(new_session_handle(o));
        std::memset(w, 0, 0x100);
        bool has_special = !ctx.out_copy.empty() || !ctx.out_move.empty();
        size_t q = 2;
        if (has_special) {
            w[q++] = (uint32_t(ctx.out_copy.size()) << 1) | (uint32_t(ctx.out_move.size()) << 5);
            for (uint32_t h : ctx.out_copy) w[q++] = h;
            for (uint32_t h : ctx.out_move) w[q++] = h;
        }
        w[q] = rc;
        std::memcpy(&w[q + 1], ctx.out.data(), ctx.out.size());
        w[0] = ctx.type;
        w[1] = uint32_t(1 + (ctx.out.size() + 3) / 4) | (has_special ? 0x80000000u : 0);
        return 0;
    }
    // Raw data: aligned to 16 bytes from the message start.
    uint64_t raw = (buf + data_start * 4 + 15) & ~15ull;
    if (g_trace)
        log("ipc: request type %u on %s (handle %x, domain %d, raw word %08x)", ctx.type, sess->svc->name.c_str(),
            handle, int(sess->domain), ld<uint32_t>(raw));
    std::shared_ptr<Service> target = sess->svc;
    uint32_t domain_cmd = 0, domain_obj = 0;
    if (sess->domain && (ctx.type == 4 || ctx.type == 6)) {
        domain_cmd = ld<uint8_t>(raw);
        uint32_t nobj = ld<uint8_t>(raw + 1);
        uint32_t dsize = ld<uint16_t>(raw + 2);
        domain_obj = ld<uint32_t>(raw + 4);
        for (uint32_t i = 0; i < nobj; i++) ctx.in_objects.push_back(ld<uint32_t>(raw + 16 + dsize + 4 * i));
        raw += 16;
        if (domain_cmd == 2) {  // close object
            sess->dom->objects.erase(domain_obj);
            goto respond_empty;
        }
        auto it = sess->dom->objects.find(domain_obj);
        if (it == sess->dom->objects.end()) {
            log("ipc: %s domain object %u not found", sess->svc->name.c_str(), domain_obj);
            return 0xF601;
        }
        target = it->second;
    }
    {
        uint32_t magic = ld<uint32_t>(raw);
        ctx.cmd = ld<uint32_t>(raw + 8);
        ctx.in = reinterpret_cast<const uint8_t*>(raw + 16);
        ctx.in_size = ndata * 4 > 32 ? ndata * 4 - 32 : 0;
        if (magic != kSFCI) {
            log("ipc: %s bad magic %08x (data_start %zu, ndata %u)", target->name.c_str(), magic, data_start, ndata);
            for (int k = 0; k < 16; k++) log("   w[%d] = %08x", k, w[k]);
        }
    }
    uint32_t rc;
    if (ctx.type == 5 || ctx.type == 7) {  // control
        switch (ctx.cmd) {
            case 0:  // ConvertCurrentObjectToDomain -> object id
                sess->domain = true;
                sess->dom->objects[sess->dom->next_object] = sess->svc;
                ctx.push<uint32_t>(sess->dom->next_object++);
                rc = 0;
                break;
            case 1:  // CopyFromCurrentDomain
                rc = 0;
                break;
            case 2:  // CloneCurrentObject
            case 4: {  // CloneCurrentObjectEx: the clone is the same session (and domain) under a new handle
                auto clone = std::make_shared<KSession>(*sess);
                ctx.out_move.push_back(handle_add(clone));
                rc = 0;
                break;
            }
            case 3:  // QueryPointerBufferSize
                ctx.push<uint16_t>(0x8000);
                rc = 0;
                break;
            default:
                log("ipc: control cmd %u", ctx.cmd);
                rc = 0;
        }
    } else {
        if (g_trace) log("ipc: %s cmd %u", target->name.c_str(), ctx.cmd);
        rc = target->handle(ctx);
    }

    {
        // Interfaces returned: domain objects, or new sessions moved to the client.
        std::vector<uint32_t> out_obj_ids;
        for (auto& o : ctx.out_objects) {
            if (sess->domain) {
                uint32_t id = sess->dom->next_object++;
                sess->dom->objects[id] = o;
                out_obj_ids.push_back(id);
            } else {
                ctx.out_move.push_back(new_session_handle(o));
            }
        }
        // Build the response.
        std::memset(w, 0, 0x100);
        bool has_special = !ctx.out_copy.empty() || !ctx.out_move.empty();
        size_t q = 2;
        if (has_special) {
            w[q++] = (uint32_t(ctx.out_copy.size()) << 1) | (uint32_t(ctx.out_move.size()) << 5);
            for (uint32_t h : ctx.out_copy) w[q++] = h;
            for (uint32_t h : ctx.out_move) w[q++] = h;
        }
        size_t dstart = q;
        uint64_t rawo = (buf + q * 4 + 15) & ~15ull;
        uint64_t o = rawo;
        if (sess->domain && (ctx.type == 4 || ctx.type == 6)) {
            st<uint32_t>(o, uint32_t(out_obj_ids.size()));
            o += 16;
        }
        st<uint32_t>(o, kSFCO);
        st<uint32_t>(o + 4, 0);
        st<uint32_t>(o + 8, rc);
        st<uint32_t>(o + 12, 0);
        std::memcpy(reinterpret_cast<void*>(o + 16), ctx.out.data(), ctx.out.size());
        uint64_t end = o + 16 + ((ctx.out.size() + 3) & ~size_t(3));
        for (uint32_t id : out_obj_ids) {
            st<uint32_t>(end, id);
            end += 4;
        }
        uint32_t words = uint32_t((end - (buf + dstart * 4)) / 4) + 4;
        w[0] = ctx.type;
        w[1] = words | (has_special ? 0x80000000u : 0);
        return 0;
    }

respond_empty:
    std::memset(w, 0, 0x100);
    w[1] = 8;
    return 0;
}

// ---- shared memory --------------------------------------------------------------------------------------------

std::shared_ptr<KSharedMemory> make_shared_memory(const std::string& name, uint64_t size) {
    auto s = std::make_shared<KSharedMemory>();
    s->name = name;
    s->size = size;
    s->host = kernel_alloc(size);
    return s;
}

uint32_t map_shared_memory(KSharedMemory& s, uint64_t addr, uint64_t size) {
    // The guest picks `addr`; we cannot alias pages cheaply, so the service keeps writing to s.host and the
    // guest reads at addr. Record the mapping: services update both through shm_sync().
    if (!commit(addr, size)) {
        log("map_shared_memory(%s): commit %llx (+%llx) failed", s.name.c_str(), (unsigned long long)addr,
            (unsigned long long)size);
        return 0xDC01;
    }
    vm_set(addr, size, 0x06 /* shared */, 3);
    std::memcpy(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(s.host), std::min(size, s.size));
    s.host = addr;  // from now on the service writes straight to the guest's view
    return 0;
}

}  // namespace nx
