#pragma once

#include <mln/extras/hooks.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/geometry.hpp>
#include <mln/math/angles.hpp>
#include <mln/math/clamp.hpp>

#include <numbers>

namespace mln {

class ProjectedMeters {
private:
    double _northing; // Distance measured northwards.
    double _easting;  // Distance measured eastwards.

public:
    ProjectedMeters(double n_ = 0, double e_ = 0)
        : _northing(n_),
          _easting(e_) {
        if (std::isnan(_northing)) {
            throw std::domain_error("northing must not be NaN");
        }
        if (std::isnan(_easting)) {
            throw std::domain_error("easting must not be NaN");
        }
    }

    double northing() const { return _northing; }
    double easting() const { return _easting; }

    friend bool operator==(const ProjectedMeters& a, const ProjectedMeters& b) {
        return a._northing == b._northing && a._easting == b._easting;
    }

    friend bool operator!=(const ProjectedMeters& a, const ProjectedMeters& b) { return !(a == b); }
};

/// Spherical Mercator projection
/// https://github.com/openlayers/docs/blob/c7ab69156e17bedc1059d64d7a864b7868e354c1/library/spherical_mercator.rst
class Projection {
public:
    // Map pixel width at given scale.
    static double worldSize(double scale) noexcept { return scale * util::tileSize_D; }

    static double getMetersPerPixelAtLatitude(double lat, double zoom) noexcept {
        const double constrainedZoom = util::clamp(zoom, util::MIN_ZOOM, util::MAX_ZOOM);
        const double constrainedScale = std::pow(2.0, constrainedZoom);
        const double constrainedLatitude = util::clamp(lat, -util::LATITUDE_MAX, util::LATITUDE_MAX);
        return std::cos(util::deg2rad(constrainedLatitude)) * util::M2PI * util::EARTH_RADIUS_M /
               worldSize(constrainedScale);
    }

    static ProjectedMeters projectedMetersForLatLng(const LatLng& latLng) noexcept {
        const double constrainedLatitude = util::clamp(latLng.latitude(), -util::LATITUDE_MAX, util::LATITUDE_MAX);
        const double constrainedLongitude = util::clamp(latLng.longitude(), -util::LONGITUDE_MAX, util::LONGITUDE_MAX);

        const double m = 1 - 1e-15;
        const double f = util::clamp(std::sin(util::deg2rad(constrainedLatitude)), -m, m);

        const double easting = util::deg2rad(util::EARTH_RADIUS_M * constrainedLongitude);
        const double northing = 0.5 * util::EARTH_RADIUS_M * std::log((1 + f) / (1 - f));

        return {northing, easting};
    }

    static LatLng latLngForProjectedMeters(const ProjectedMeters& projectedMeters) {
        double latitude = util::rad2deg(2 * std::atan(std::exp(projectedMeters.northing() / util::EARTH_RADIUS_M)) -
                                        (std::numbers::pi / 2.0));
        double longitude = util::rad2deg(projectedMeters.easting()) / util::EARTH_RADIUS_M;

        latitude = util::clamp(latitude, -util::LATITUDE_MAX, util::LATITUDE_MAX);
        longitude = util::clamp(longitude, -util::LONGITUDE_MAX, util::LONGITUDE_MAX);

        return {latitude, longitude};
    }

    static Point<double> project(const LatLng& latLng, double scale) noexcept {
        return project_(latLng, worldSize(scale));
    }

    /// Returns point on tile
    static Point<double> project(const LatLng& latLng, int32_t zoom) noexcept { return project_(latLng, 1 << zoom); }

    static LatLng unproject(const Point<double>& p, double scale, LatLng::WrapMode wrapMode = LatLng::Unwrapped) {
        auto p2 = p * util::DEGREES_MAX / worldSize(scale);
        double lat;
        double lng;
        // hook:平面投影可被整体替换(如 BD09MC);未替换时用引擎原生 Web 墨卡托
        if (const auto* proj = extras::planarProjection()) {
            const double ws = worldSize(scale);
            proj->unproject(p.x / ws, p.y / ws, lng, lat);
        } else {
            lat = std::atan(std::exp(util::deg2rad(util::LONGITUDE_MAX - p2.y))) * util::DEGREES_MAX / M_PI - 90.0;
            lng = p2.x - util::LONGITUDE_MAX;
        }
        // hook:瓦片基准面(GCJ-02/BD09)→ WGS-84,见 src/mln/extras/offset.cpp
        if (const auto* shift = extras::datumShift()) shift->inverse(lng, lat);
        return LatLng{lat, lng, wrapMode};
    }

private:
    static Point<double> project_(const LatLng& latLng, double worldSize) noexcept {
        double lng = latLng.longitude();
        double lat = latLng.latitude();
        // hook:WGS-84 → 瓦片基准面,之后照常走原生 Web 墨卡托
        if (const auto* shift = extras::datumShift()) shift->forward(lng, lat);
        if (const auto* proj = extras::planarProjection()) {
            double nx;
            double ny;
            proj->project(lng, lat, nx, ny);
            return Point<double>{nx * worldSize, ny * worldSize};
        }
        const double latitude = util::clamp(lat, -util::LATITUDE_MAX, util::LATITUDE_MAX);
        return Point<double>{util::LONGITUDE_MAX + lng,
                             util::LONGITUDE_MAX -
                                 util::rad2deg(std::log(std::tan(M_PI / 4 + latitude * M_PI / util::DEGREES_MAX)))} *
               (worldSize / util::DEGREES_MAX);
    }
};

} // namespace mln
