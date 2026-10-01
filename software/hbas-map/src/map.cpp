// SPDX-License-Identifier: MIT
// libosmscout behind the C interface in hbas/map.h. The sequence (open
// database, load style, project, load tiles, AGG painter) follows
// libosmscout's documented API and its DrawMapAgg demo.
#include "hbas/map.h"

#include <cmath>
#include <cstdio>
#include <chrono>
#include <list>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include <osmscout/db/Database.h>
#include <osmscout/projection/MercatorProjection.h>
#include <osmscout/routing/RoutePostprocessor.h>
#include <osmscout/routing/RoutingProfile.h>
#include <osmscout/routing/SimpleRoutingService.h>
#include <osmscoutmap/MapService.h>
#include <osmscoutmap/StyleConfig.h>
#include <osmscoutmapagg/MapPainterAgg.h>

#include <agg_conv_stroke.h>
#include <agg_path_storage.h>
#include <agg_rasterizer_scanline_aa.h>
#include <agg_renderer_scanline.h>
#include <agg_scanline_u.h>

struct hbas_map {
	osmscout::DatabaseRef database;
	osmscout::MapServiceRef service;
	osmscout::StyleConfigRef style;
	osmscout::MapParameter parameter;
	osmscout::AreaSearchParameter search;
	osmscout::MercatorProjection projection;
	std::vector<unsigned char> rgb;          // AGG draws 24-bit RGB
	osmscout::SimpleRoutingServiceRef router;  // opened on the first route
	std::vector<osmscout::GeoCoord> route;     // drawn in orange
	double dpi = 96;
	bool rendered = false;
};

extern "C" struct hbas_map *hbas_map_open(const char *db_dir, const char *style,
					  const char *font, double dpi, char *err,
					  size_t errlen)
{
	auto fail = [&](const char *why) -> struct hbas_map * {
		if (err && errlen)
			std::snprintf(err, errlen, "%s", why);
		return nullptr;
	};

	try {
		auto m = std::make_unique<hbas_map>();
		osmscout::DatabaseParameter dbp;

		m->database = std::make_shared<osmscout::Database>(dbp);
		if (!m->database->Open(db_dir))
			return fail("cannot open the map database");
		m->service = std::make_shared<osmscout::MapService>(m->database);
		m->style = std::make_shared<osmscout::StyleConfig>(m->database->GetTypeConfig());
		if (!m->style->Load(style))
			return fail("cannot load the map style");
		m->dpi = dpi > 0 ? dpi : 96;
		m->parameter.SetFontName(font);
		m->parameter.SetFontSize(3.0);
		m->parameter.SetRenderSeaLand(true);
		m->parameter.SetRenderUnknowns(false);
		m->parameter.SetRenderBackground(false);  // we fill with the land colour (works rotated)
		m->parameter.SetLabelLineMinCharCount(15);
		m->parameter.SetLabelLineMaxCharCount(30);
		m->parameter.SetLabelLineFitToArea(true);
		return m.release();
	} catch (const std::exception &e) {
		return fail(e.what());
	}
}

extern "C" void hbas_map_close(struct hbas_map *m)
{
	delete m;
}

// The route over the map: a dark casing, then the orange line (COL_ACCENT).
static void draw_route(hbas_map *m, agg::pixfmt_rgb24 &pf)
{
	if (m->route.size() < 2)
		return;
	agg::path_storage path;
	bool first = true;

	for (const auto &c : m->route) {
		osmscout::Vertex2D v;

		if (!m->projection.GeoToPixel(c, v))
			continue;
		if (first)
			path.move_to(v.GetX(), v.GetY());
		else
			path.line_to(v.GetX(), v.GetY());
		first = false;
	}
	agg::renderer_base<agg::pixfmt_rgb24> base(pf);
	agg::renderer_scanline_aa_solid<agg::renderer_base<agg::pixfmt_rgb24>> ren(base);
	agg::rasterizer_scanline_aa<> ras;
	agg::scanline_u8 sl;
	agg::conv_stroke<agg::path_storage> stroke(path);

	stroke.line_join(agg::round_join);
	stroke.line_cap(agg::round_cap);
	const struct { double w; agg::rgba8 c; } layers[] = {
		{ 9.0, agg::rgba8(0x10, 0x10, 0x12) },
		{ 5.5, agg::rgba8(0xFF, 0x7A, 0x1A) },
	};
	for (const auto &l : layers) {
		stroke.width(l.w);
		ras.reset();
		ras.add_path(stroke);
		ren.color(l.c);
		agg::render_scanlines(ras, sl, ren);
	}
}

extern "C" int hbas_map_render(struct hbas_map *m, double lat, double lon, double level,
			       double rotation_deg, uint16_t *out, int w, int h)
{
	if (!m || !out || w <= 0 || h <= 0)
		return -1;
	try {
		osmscout::Magnification mag;
		osmscout::MapData data;
		std::list<osmscout::TileRef> tiles;
		std::vector<osmscout::MapData> all;

		mag.SetMagnification(std::pow(2.0, level));
		// osmscout's angle turns the map clockwise; heading-up wants the
		// direction of travel at the top, i.e. turn the world by -heading
		if (!m->projection.Set(osmscout::GeoCoord(lat, lon),
				       -rotation_deg * M_PI / 180.0, mag, m->dpi,
				       (size_t)w, (size_t)h))
			return -1;
		data.styleConfig = m->style;
		m->service->LookupTiles(m->projection, tiles);
		m->service->LoadMissingTileData(m->search, *m->style, tiles);
		m->service->AddTileDataToMapData(tiles, data);
		m->service->GetGroundTiles(m->projection, data.groundTiles);
		all.push_back(std::move(data));

		// land colour first (hogtied.oss landColor #1b1c20); water and
		// roads are drawn over it
		m->rgb.resize((size_t)w * h * 3);
		for (size_t i = 0; i < m->rgb.size(); i += 3) {
			m->rgb[i] = 0x1b;
			m->rgb[i + 1] = 0x1c;
			m->rgb[i + 2] = 0x20;
		}
		agg::rendering_buffer rbuf(m->rgb.data(), (unsigned)w, (unsigned)h, w * 3);
		agg::pixfmt_rgb24 pf(rbuf);
		// a fresh painter per frame: a reused one keeps its label layout and
		// leaves street names out of the next frame (seen in testing)
		osmscout::MapPainterAgg painter;

		if (!painter.DrawMap(m->projection, m->parameter, all, &pf))
			return -1;
		draw_route(m, pf);
		for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
			const unsigned char *p = &m->rgb[i * 3];

			out[i] = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
		}
		m->rendered = true;
		return 0;
	} catch (const std::exception &) {
		return -1;
	}
}

extern "C" bool hbas_map_to_pixel(const struct hbas_map *m, double lat, double lon, double *x,
				  double *y)
{
	if (!m || !m->rendered)
		return false;
	osmscout::Vertex2D v;

	if (!m->projection.GeoToPixel(osmscout::GeoCoord(lat, lon), v))
		return false;
	*x = v.GetX();
	*y = v.GetY();
	return true;
}

extern "C" const char *hbas_route_mode_name(enum hbas_route_mode mode)
{
	switch (mode) {
	case HBAS_ROUTE_FASTEST: return "Fastest";
	case HBAS_ROUTE_SHORTEST: return "Shortest";
	case HBAS_ROUTE_NO_HIGHWAYS: return "No highways";
	case HBAS_ROUTE_BACKROADS: return "Back roads";
	default: return "?";
	}
}

// Typical speeds (km/h) by road type, for the fastest route and for time
// estimates; libosmscout's Routing demo uses the same table.
static std::map<std::string, double> car_speeds()
{
	return {
		{ "highway_motorway", 110 }, { "highway_motorway_trunk", 100 },
		{ "highway_motorway_primary", 70 }, { "highway_motorway_link", 60 },
		{ "highway_motorway_junction", 60 }, { "highway_trunk", 100 },
		{ "highway_trunk_link", 60 }, { "highway_primary", 70 },
		{ "highway_primary_link", 60 }, { "highway_secondary", 60 },
		{ "highway_secondary_link", 50 }, { "highway_tertiary", 55 },
		{ "highway_tertiary_link", 55 }, { "highway_unclassified", 50 },
		{ "highway_road", 50 }, { "highway_residential", 20 },
		{ "highway_roundabout", 40 }, { "highway_living_street", 10 },
		{ "highway_service", 30 }, { "highway_mini_roundabout", 30 },
	};
}

static bool is_motorway(const std::string &t)
{
	return t.rfind("highway_motorway", 0) == 0;
}

// The speed table for a mode. Types a car can use that aren't listed
// (ferries and the like) get a slow default, so they're used only when
// needed; types left out entirely can't be routed over.
static std::map<std::string, double> mode_speeds(const osmscout::TypeConfig &tc,
						 enum hbas_route_mode mode)
{
	std::map<std::string, double> base = car_speeds(), out;

	for (const auto &type : tc.GetTypes()) {
		if (type->GetIgnore() || !type->CanRouteCar())
			continue;
		const std::string &n = type->GetName();
		auto it = base.find(n);
		double v = it != base.end() ? it->second : 20;

		if (mode == HBAS_ROUTE_NO_HIGHWAYS && is_motorway(n))
			continue;
		if (mode == HBAS_ROUTE_BACKROADS) {
			// big roads made "slow" so the router prefers the
			// country roads between them, but can still use them
			if (is_motorway(n))
				v = 35;
			else if (n.rfind("highway_trunk", 0) == 0)
				v = 40;
			else if (n == "highway_primary")
				v = 50;
			else if (n == "highway_secondary" || n == "highway_tertiary")
				v = 70;
			else if (n == "highway_unclassified")
				v = 60;
		}
		out[n] = v;
	}
	return out;
}

extern "C" void hbas_map_clear_route(struct hbas_map *m)
{
	if (m)
		m->route.clear();
}

extern "C" int hbas_map_route(struct hbas_map *m, double from_lat, double from_lon,
			      double heading_deg, double to_lat, double to_lon,
			      enum hbas_route_mode mode, struct hbas_route_info *info, char *err,
			      size_t errlen)
{
	auto fail = [&](const char *why) {
		if (err && errlen)
			std::snprintf(err, errlen, "%s", why);
		return -1;
	};

	if (!m || mode < 0 || mode >= HBAS_ROUTE_MODES)
		return fail("bad request");
	try {
		const osmscout::TypeConfig &tc = *m->database->GetTypeConfig();

		if (!m->router) {
			osmscout::RouterParameter rp;
			auto r = std::make_shared<osmscout::SimpleRoutingService>(
				m->database, rp, osmscout::RoutingService::DEFAULT_FILENAME_BASE);

			if (!r->Open())
				return fail("this map has no routing data");
			m->router = r;
		}

		// time estimates always use the real road speeds
		auto timing = std::make_shared<osmscout::FastestPathRoutingProfile>(
			m->database->GetTypeConfig());
		timing->ParametrizeForCar(tc, mode_speeds(tc, HBAS_ROUTE_FASTEST), 160.0);

		std::shared_ptr<osmscout::AbstractRoutingProfile> profile;
		if (mode == HBAS_ROUTE_SHORTEST)
			profile = std::make_shared<osmscout::ShortestPathRoutingProfile>(
				m->database->GetTypeConfig());
		else
			profile = std::make_shared<osmscout::FastestPathRoutingProfile>(
				m->database->GetTypeConfig());
		// set up by hand, not ParametrizeForCar: that complains about the
		// road types "No highways" leaves out on purpose
		profile->SetVehicle(osmscout::vehicleCar);
		profile->SetVehicleMaxSpeed(160.0);
		for (const auto &[name, kmh] : mode_speeds(tc, mode))
			profile->AddType(tc.GetTypeInfo(name), kmh);

		auto start = m->router->GetClosestRoutableNode(
			osmscout::GeoCoord(from_lat, from_lon), *profile, osmscout::Kilometers(1));
		if (!start.IsValid())
			return fail("no road near you");
		auto target = m->router->GetClosestRoutableNode(
			osmscout::GeoCoord(to_lat, to_lon), *profile, osmscout::Kilometers(1));
		if (!target.IsValid())
			return fail("no road near the destination");

		std::optional<osmscout::Bearing> bearing;
		if (heading_deg >= 0)
			bearing = osmscout::Bearing::Degrees(heading_deg);
		osmscout::RoutingParameter param;
		auto result = m->router->CalculateRoute(*profile, start.GetRoutePosition(),
							target.GetRoutePosition(), bearing, param);
		if (!result.Success())
			return fail("no route found");

		auto points = m->router->TransformRouteDataToPoints(result.GetRoute());
		auto desc = m->router->TransformRouteDataToRouteDescription(result.GetRoute());
		if (!points.Success() || !desc.Success())
			return fail("route could not be read");

		osmscout::RoutePostprocessor post;
		std::list<osmscout::RoutePostprocessor::PostprocessorRef> steps{
			std::make_shared<osmscout::RoutePostprocessor::DistanceAndTimePostprocessor>(),
		};
		std::vector<osmscout::RoutingProfileRef> profiles{ timing };
		std::vector<osmscout::DatabaseRef> dbs{ m->database };
		if (!post.PostprocessRouteDescription(*desc.GetDescription(), profiles, dbs, steps))
			return fail("route could not be measured");

		std::vector<osmscout::GeoCoord> line;
		for (const auto &p : points.GetPoints()->points)
			line.push_back(p.GetCoord());
		if (line.size() < 2)
			return fail("you are already there");
		m->route = std::move(line);

		if (info) {
			const auto &last = desc.GetDescription()->Nodes().back();

			info->distance_m = last.GetDistance().AsMeter();
			info->duration_s = std::chrono::duration<double>(last.GetTime()).count();
			info->points = m->route.size();
		}
		return 0;
	} catch (const std::exception &e) {
		return fail(e.what());
	}
}
