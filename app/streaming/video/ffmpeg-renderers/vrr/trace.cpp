#include "trace.h"

#include <cstdio>
#include <cstring>

namespace Vrr {
Trace::Trace() : m_Records(Capacity) {}

void Trace::append(Record r)
{
    r.serial = ++m_Serial;
    m_Records[(r.serial - 1) % Capacity] = r;
}

void Trace::add(Event event, uint64_t id, Ns at, std::initializer_list<int64_t> data)
{
    Record r;
    r.event = event; r.id = id; r.at = at;
    std::copy_n(data.begin(), std::min(data.size(), r.data.size()), r.data.begin());
    std::lock_guard<std::mutex> lock(m_Lock);
    append(r);
}

void Trace::checkpoint(const Controller& controller, Ns at)
{
    auto words = controller.checkpoint();
    std::lock_guard<std::mutex> lock(m_Lock);
    Record r;
    r.event = Event::Build; r.at = at;
    std::copy(m_BuildHash.begin(), m_BuildHash.end(), r.data.begin());
    append(r);
    r.event = Event::CheckpointBegin; r.at = at; r.data[0] = int64_t(words.size());
    std::fill(r.data.begin() + 1, r.data.end(), 0);
    append(r);
    for (size_t offset = 0; offset < words.size(); offset += r.data.size()) {
        r.event = Event::CheckpointData;
        r.data = {};
        std::copy_n(words.begin() + offset, std::min(r.data.size(), words.size() - offset), r.data.begin());
        append(r);
    }
    r.event = Event::CheckpointEnd; r.data = {};
    append(r);
}

static bool put(FILE* file, uint64_t value)
{
    unsigned char bytes[8];
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (value >> (8 * i)) & 255;
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static bool get(FILE* file, uint64_t& value)
{
    unsigned char bytes[8];
    if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) return false;
    value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (8 * i);
    return true;
}

bool Trace::save(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_Lock);
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return false;
    const uint64_t count = std::min<uint64_t>(m_Serial, Capacity);
    bool ok = fwrite("MLVRR14\0", 1, 8, file) == 8 && put(file, 1) && put(file, count) && put(file, m_Serial - count);
    for (uint64_t serial = m_Serial - count + 1; ok && serial <= m_Serial; ++serial) {
        const auto& r = m_Records[(serial - 1) % Capacity];
        ok = put(file, r.serial) && put(file, uint64_t(r.event)) && put(file, r.id) && put(file, uint64_t(r.at));
        for (auto word : r.data) ok = put(file, uint64_t(word)) && ok;
    }
    ok = fclose(file) == 0 && ok;
    return ok;
}

bool Trace::read(const std::string& path, std::vector<Record>& records)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    char magic[8];
    uint64_t version = 0, count = 0, lost = 0;
    bool ok = fread(magic, 1, 8, file) == 8 && !memcmp(magic, "MLVRR14\0", 8) &&
        get(file, version) && version == 1 && get(file, count) && count <= Capacity && get(file, lost);
    records.clear();
    for (uint64_t i = 0; ok && i < count; ++i) {
        Record r;
        uint64_t type = 0, at = 0, word = 0;
        ok = get(file, r.serial) && get(file, type) && get(file, r.id) && get(file, at);
        r.event = Event(type); r.at = int64_t(at);
        ok = ok && r.serial == lost + i + 1 && type >= 1 && type <= uint64_t(Event::Reserve);
        for (auto& value : r.data) { ok = get(file, word) && ok; value = int64_t(word); }
        if (ok) records.push_back(r);
    }
    ok = ok && fgetc(file) == EOF;
    fclose(file);
    if (!ok) records.clear();
    return ok;
}
}
