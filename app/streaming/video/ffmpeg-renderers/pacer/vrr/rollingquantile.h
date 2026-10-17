#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace Vrr13 {

// Experimental exact segmented index. Bounds the ordinary shifted suffix to
// one small block; retained values and nearest-rank semantics are unchanged.
class ExactRollingQuantile {
    using Block = std::vector<int64_t>;
    static constexpr size_t BlockMaximum = 512;
    static constexpr size_t BlockMinimum = 128;
public:
    ExactRollingQuantile() = default;
    ExactRollingQuantile(const ExactRollingQuantile&) = default;
    ExactRollingQuantile& operator=(const ExactRollingQuantile&) = default;
    ExactRollingQuantile(ExactRollingQuantile&& other) noexcept
        : m_Blocks(std::move(other.m_Blocks)), m_Size(std::exchange(other.m_Size, 0))
    { other.m_Blocks.clear(); }
    ExactRollingQuantile& operator=(ExactRollingQuantile&& other) noexcept
    {
        if (this != &other) {
            m_Blocks = std::move(other.m_Blocks);
            m_Size = std::exchange(other.m_Size, 0);
            other.m_Blocks.clear();
        }
        return *this;
    }
    void clear() { m_Blocks.clear(); m_Size = 0; }
    size_t size() const { return m_Size; }
    bool empty() const { return m_Size == 0; }

    void add(int64_t value)
    {
        if (m_Blocks.empty()) m_Blocks.emplace_back();
        size_t index = locate(value);
        if (index == m_Blocks.size()) --index;
        auto& block = m_Blocks[index];
        block.insert(std::upper_bound(block.begin(), block.end(), value), value);
        ++m_Size;
        if (block.size() > BlockMaximum) {
            const size_t middle = block.size() / 2;
            Block right(block.begin() + middle, block.end());
            Block left(block.begin(), block.begin() + middle);
            block = std::move(left);
            m_Blocks.insert(m_Blocks.begin() + index + 1, std::move(right));
        }
    }
    bool remove(int64_t value)
    {
        const size_t index = locate(value);
        if (index == m_Blocks.size()) return false;
        auto& block = m_Blocks[index];
        const auto at = std::lower_bound(block.begin(), block.end(), value);
        if (at == block.end() || *at != value) return false;
        block.erase(at);
        --m_Size;
        repair(index);
        return true;
    }
    void removeBatch(std::vector<int64_t> removed)
    {
        if (removed.empty() || empty()) return;
        // Small expiry bursts touch fewer blocks than a full index scan.
        // Keep large retirements on the single-pass compaction path.
        if (removed.size() < m_Blocks.size()) {
            for (const auto value : removed) remove(value);
            return;
        }
        if (!std::is_sorted(removed.begin(), removed.end()))
            std::sort(removed.begin(), removed.end());
        size_t erase = 0;
        for (auto& block : m_Blocks) {
            size_t write = 0;
            for (size_t read = 0; read < block.size(); ++read) {
                while (erase < removed.size() && removed[erase] < block[read]) ++erase;
                if (erase < removed.size() && removed[erase] == block[read]) {
                    ++erase;
                    --m_Size;
                }
                else block[write++] = block[read];
            }
            block.resize(write);
        }
        m_Blocks.erase(std::remove_if(m_Blocks.begin(), m_Blocks.end(),
            [](const Block& block) { return block.empty(); }), m_Blocks.end());
        for (size_t i = 0; i < m_Blocks.size();) {
            if (m_Blocks.size() > 1 && m_Blocks[i].size() < BlockMinimum) {
                const size_t left = i + 1 < m_Blocks.size() ? i : i - 1;
                combine(left);
                i = left;
            }
            else ++i;
        }
    }
    int64_t atRank(size_t rank) const
    {
        if (empty()) return 0;
        rank = std::clamp<size_t>(rank, 1, size());
        if (rank <= size() / 2) {
            for (const auto& block : m_Blocks) {
                if (rank <= block.size()) return block[rank - 1];
                rank -= block.size();
            }
        }
        else {
            size_t offset = size() - rank;
            for (auto it = m_Blocks.rbegin(); it != m_Blocks.rend(); ++it) {
                if (offset < it->size()) return (*it)[it->size() - 1 - offset];
                offset -= it->size();
            }
        }
        return 0;
    }
private:
    size_t locate(int64_t value) const
    {
        if (m_Size == 0) return 0;
        return size_t(std::lower_bound(m_Blocks.begin(), m_Blocks.end(), value,
            [](const Block& block, int64_t v) { return block.back() < v; }) - m_Blocks.begin());
    }
    void combine(size_t left)
    {
        Block values;
        values.reserve(m_Blocks[left].size() + m_Blocks[left + 1].size());
        values.insert(values.end(), m_Blocks[left].begin(), m_Blocks[left].end());
        values.insert(values.end(), m_Blocks[left + 1].begin(), m_Blocks[left + 1].end());
        if (values.size() > BlockMaximum) {
            const size_t middle = values.size() / 2;
            m_Blocks[left + 1].assign(values.begin() + middle, values.end());
            Block first(values.begin(), values.begin() + middle);
            m_Blocks[left] = std::move(first);
        }
        else {
            m_Blocks[left] = std::move(values);
            m_Blocks.erase(m_Blocks.begin() + left + 1);
        }
    }
    void repair(size_t index)
    {
        if (m_Blocks[index].empty()) {
            m_Blocks.erase(m_Blocks.begin() + index);
            return;
        }
        if (m_Blocks.size() > 1 && m_Blocks[index].size() < BlockMinimum)
            combine(index + 1 < m_Blocks.size() ? index : index - 1);
    }
    std::vector<Block> m_Blocks;
    size_t m_Size = 0;
};
} // namespace Vrr13
