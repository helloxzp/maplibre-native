#pragma once

#include <mln/extras/extras.hpp>

#include <atomic>
#include <string>
#include <vector>

namespace mln {
namespace extras {

/// `extra: { "offset": "gcj02" }` 或 `{ "offset": "bd09" }`
///
/// 声明瓦片所用的经纬度基准面。国内地图数据普遍不是 WGS-84:高德/腾讯用
/// GCJ-02(国测局"火星坐标"),百度在 GCJ-02 之上再叠一层 BD09 位移。引擎
/// 对外 API 仍然收发 WGS-84,由本扩展在"经纬度 ↔ 墨卡托"这一层做换算。
///
/// **只做偏移**:瓦片编号、投影公式、渲染管线一律保持原生 Web 墨卡托,
/// 因此适用于以标准 XYZ 切片、但坐标基准面被偏移过的数据源。
///
/// 算法与两个代理端实现保持一致(gaode-mvt-proxy / baidu-mvt-proxy),
/// 包括各自的"境外不偏移"判定范围——判定范围不同会在国境线附近产生
/// 半个瓦片的错位,必须与生成瓦片的那一端逐字对齐。
class OffsetExtension final : public Extension, private DatumShift {
public:
    enum class Datum : uint8_t {
        None,  ///< 不偏移,等同原版
        GCJ02, ///< 高德/腾讯
        BD09,  ///< 百度(GCJ-02 之上再叠一层)
    };

    std::vector<std::string> keys() const override { return {"offset"}; }
    bool configure(const std::string& key, const JSValue& value) override;
    void reset() override;
    DatumShift* datumShift() noexcept override {
        return active.load(std::memory_order_relaxed) == Datum::None ? nullptr : this;
    }

private:
    void forward(double& lng, double& lat) const noexcept override;
    void inverse(double& lng, double& lat) const noexcept override;

    std::atomic<Datum> active{Datum::None};
};

} // namespace extras
} // namespace mln
