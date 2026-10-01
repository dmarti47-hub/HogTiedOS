// SPDX-License-Identifier: MIT
// libosmscout behind the C interface in hbas/map.h. The sequence (open
// database, load style, project, load tiles, AGG painter) follows
// libosmscout's documented API and its DrawMapAgg demo.
#include "hbas/map.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <osmscout/db/Database.h>
#include <osmscout/location/LocationService.h>
#include <osmscout/projection/MercatorProjection.h>
#include <osmscout/util/StringMatcher.h>
#include <osmscout/routing/RouteDescriptionPostprocessor.h>
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
	osmscout::LocationServiceRef locations;   // opened on the first search
	// every town (region alias) and its region, to know which region the
	// bike is in: loaded on the first search
	std::vector<std::pair<osmscout::GeoCoord, osmscout::AdminRegionRef>> towns;
	std::vector<osmscout::GeoCoord> route;     // drawn in orange
	std::vector<hbas_route_step> steps;        // its manoeuvres
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


// The route's manoeuvres, from libosmscout's description generator.
namespace {
using RD = osmscout::RouteDescription;

hbas_turn from_move(RD::DirectionDescription::Move mv)
{
	switch (mv) {
	case RD::DirectionDescription::sharpLeft: return HBAS_TURN_SHARP_LEFT;
	case RD::DirectionDescription::left: return HBAS_TURN_LEFT;
	case RD::DirectionDescription::slightlyLeft: return HBAS_TURN_SLIGHT_LEFT;
	case RD::DirectionDescription::slightlyRight: return HBAS_TURN_SLIGHT_RIGHT;
	case RD::DirectionDescription::right: return HBAS_TURN_RIGHT;
	case RD::DirectionDescription::sharpRight: return HBAS_TURN_SHARP_RIGHT;
	default: return HBAS_TURN_STRAIGHT;
	}
}

bool leftish(const RD::DirectionDescriptionRef &d)
{
	if (!d)
		return false;
	auto t = d->GetCurve();
	return t == RD::DirectionDescription::sharpLeft || t == RD::DirectionDescription::left ||
	       t == RD::DirectionDescription::slightlyLeft;
}

// Motorways by number ("I 39/I 90"), other roads by name ("Main Street").
std::string road_name(const RD::NameDescriptionRef &n, bool prefer_ref = false)
{
	if (!n)
		return "";
	std::string name = n->GetName(), ref = n->GetRef();

	for (auto &c : ref)
		if (c == ';')
			c = '/';
	if (prefer_ref)
		return !ref.empty() ? ref : name;
	return !name.empty() ? name : ref;
}

struct steps_cb : osmscout::RouteDescriptionPostprocessor::Callback {
	std::vector<hbas_route_step> out;
	double dist = 0;
	osmscout::GeoCoord at;

	void add(hbas_turn t, const std::string &name, int exit = 0)
	{
		hbas_route_step s{};

		s.turn = t;
		s.exit = exit;
		s.dist_m = dist;
		s.lat = at.GetLat();
		s.lon = at.GetLon();
		std::snprintf(s.name, sizeof(s.name), "%s", name.c_str());
		// a roundabout's exit is described when leaving: merge
		if (t == HBAS_TURN_ROUNDABOUT && !out.empty() && out.back().turn == t &&
		    out.back().exit == 0) {
			out.back().exit = exit;
			std::snprintf(out.back().name, sizeof(out.back().name), "%s", name.c_str());
			return;
		}
		out.push_back(s);
	}
	void BeforeNode(const RD::Node &node) override
	{
		dist = node.GetDistance().AsMeter();
		at = node.GetLocation();
	}
	void OnTargetReached(const RD::TargetDescriptionRef &) override
	{
		add(HBAS_TURN_ARRIVE, "");
	}
	void OnTurn(const RD::TurnDescriptionRef &turn, const RD::CrossingWaysDescriptionRef &,
		    const RD::DirectionDescriptionRef &, const RD::TypeNameDescriptionRef &,
		    const RD::NameDescriptionRef &name) override
	{
		add(from_move(turn->GetDirection()), road_name(name));
	}
	void OnRoundaboutEnter(const RD::RoundaboutEnterDescriptionRef &,
			       const RD::CrossingWaysDescriptionRef &) override
	{
		add(HBAS_TURN_ROUNDABOUT, "", 0);
	}
	void OnRoundaboutLeave(const RD::RoundaboutLeaveDescriptionRef &leave,
			       const RD::NameDescriptionRef &name) override
	{
		add(HBAS_TURN_ROUNDABOUT, road_name(name), (int)leave->GetExitCount());
	}
	void OnMotorwayEnter(const RD::MotorwayEnterDescriptionRef &enter,
			     const RD::CrossingWaysDescriptionRef &) override
	{
		add(HBAS_TURN_MOTORWAY_ENTER, road_name(enter->GetToDescription(), true));
	}
	void OnMotorwayChange(const RD::MotorwayChangeDescriptionRef &change,
			      const RD::MotorwayJunctionDescriptionRef &,
			      const RD::DirectionDescriptionRef &dir,
			      const RD::DestinationDescriptionRef &) override
	{
		add(leftish(dir) ? HBAS_TURN_KEEP_LEFT : HBAS_TURN_KEEP_RIGHT,
		    road_name(change->GetToDescription(), true));
	}
	void OnMotorwayLeave(const RD::MotorwayLeaveDescriptionRef &,
			     const RD::MotorwayJunctionDescriptionRef &,
			     const RD::DirectionDescriptionRef &dir, const RD::NameDescriptionRef &name,
			     const RD::DestinationDescriptionRef &dest) override
	{
		std::string to = road_name(name);

		// unnamed ramp: say where it goes, as the signs do
		if (to.empty() && dest)
			to = dest->GetDescription();
		add(leftish(dir) ? HBAS_TURN_MOTORWAY_EXIT_LEFT : HBAS_TURN_MOTORWAY_EXIT_RIGHT, to);
	}
};
} // namespace

static std::vector<hbas_route_step> describe(RD &desc)
{
	osmscout::RouteDescriptionPostprocessor gen;
	steps_cb cb;

	gen.GenerateDescription(desc, cb);
	// two manoeuvres a few metres apart (a slight bend onto a ramp, then
	// the ramp) are one for the rider: keep the second
	std::vector<hbas_route_step> out;
	for (const auto &st : cb.out) {
		if (!out.empty() && st.dist_m - out.back().dist_m < 30 &&
		    out.back().turn != HBAS_TURN_ROUNDABOUT)
			out.back() = st;
		else
			out.push_back(st);
	}
	return out;
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

extern "C" size_t hbas_map_route_points(const struct hbas_map *m, double *lat, double *lon,
				       size_t max)
{
	if (!m)
		return 0;
	for (size_t i = 0; i < m->route.size() && i < max; i++) {
		lat[i] = m->route[i].GetLat();
		lon[i] = m->route[i].GetLon();
	}
	return m->route.size();
}

extern "C" void hbas_map_clear_route(struct hbas_map *m)
{
	if (m) {
		m->route.clear();
		m->steps.clear();
	}
}

extern "C" size_t hbas_map_route_steps(const struct hbas_map *m, struct hbas_route_step *out,
				       size_t max)
{
	if (!m)
		return 0;
	for (size_t i = 0; i < m->steps.size() && i < max; i++)
		out[i] = m->steps[i];
	return m->steps.size();
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

		using RP = osmscout::RoutePostprocessor;
		RP post;
		std::list<RP::PostprocessorRef> steps{
			std::make_shared<RP::DistanceAndTimePostprocessor>(),
			std::make_shared<RP::StartPostprocessor>("Start"),
			std::make_shared<RP::TargetPostprocessor>("Target"),
			std::make_shared<RP::WayNamePostprocessor>(),
			std::make_shared<RP::WayTypePostprocessor>(),
			std::make_shared<RP::CrossingWaysPostprocessor>(),
			std::make_shared<RP::DirectionPostprocessor>(),
			std::make_shared<RP::MotorwayJunctionPostprocessor>(),
			std::make_shared<RP::DestinationPostprocessor>(),
			std::make_shared<RP::InstructionPostprocessor>(),
		};
		std::vector<osmscout::RoutingProfileRef> profiles{ timing };
		std::vector<osmscout::DatabaseRef> dbs{ m->database };
		const std::set<std::string, std::less<>> motorways{
			"highway_motorway", "highway_motorway_trunk", "highway_motorway_primary",
			"highway_trunk" },
			links{ "highway_motorway_link", "highway_trunk_link" },
			junctions{ "highway_motorway_junction" };
		if (!post.PostprocessRouteDescription(*desc.GetDescription(), profiles, dbs, steps,
						      motorways, links, junctions))
			return fail("route could not be measured");
		std::vector<hbas_route_step> manoeuvres = describe(*desc.GetDescription());

		std::vector<osmscout::GeoCoord> line;
		for (const auto &p : points.GetPoints()->points)
			line.push_back(p.GetCoord());
		if (line.size() < 2)
			return fail("you are already there");
		m->route = std::move(line);
		m->steps = std::move(manoeuvres);

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


static std::string lower(std::string s)
{
	for (auto &c : s)
		c = (char)std::tolower((unsigned char)c);
	return s;
}

// Riders type short forms; OpenStreetMap spells names out.
static std::string expand_abbreviations(const char *query)
{
	static const std::map<std::string, std::string> words = {
		{ "st", "street" }, { "ave", "avenue" }, { "av", "avenue" }, { "rd", "road" },
		{ "dr", "drive" }, { "ln", "lane" }, { "ct", "court" }, { "blvd", "boulevard" },
		{ "pkwy", "parkway" }, { "hwy", "highway" }, { "pl", "place" }, { "cir", "circle" },
		{ "ter", "terrace" }, { "trl", "trail" }, { "n", "north" }, { "s", "south" },
		{ "e", "east" }, { "w", "west" }, { "mt", "mount" }, { "ft", "fort" },
	};
	std::string out, word;
	auto flush = [&]() {
		auto it = words.find(lower(word));

		if (!word.empty())
			out += (out.empty() ? "" : " ") + (it != words.end() ? it->second : word);
		word.clear();
	};

	for (const char *c = query; *c; c++) {
		if (*c == ' ' || *c == ',')
			flush();
		else
			word += *c;
	}
	flush();
	// "400 west canal street" -> "west canal street 400": the index
	// expects the house number after the street
	if (!out.empty() && std::isdigit((unsigned char)out[0])) {
		size_t sp = out.find(' ');

		if (sp != std::string::npos)
			out = out.substr(sp + 1) + " " + out.substr(0, sp);
	}
	return out;
}

// How many words of the query start a word of name (any case)?
static int count_matching_words(const std::string &name, const std::string &query)
{
	std::string n = " " + lower(name) + " ", q = lower(query) + " ";
	int count = 0;

	for (size_t i = 0, j; (j = q.find(' ', i)) != std::string::npos; i = j + 1)
		if (j > i && n.find(" " + q.substr(i, j - i)) != std::string::npos)
			count++;
	return count;
}

// Does some word of name start with some word of the query (any case)?
static bool words_start_with(const std::string &name, const std::string &query)
{
	std::string n = " " + lower(name), q = lower(query);
	size_t i = 0;

	while (i < q.size()) {
		size_t j = q.find(' ', i);
		std::string w = q.substr(i, j == std::string::npos ? std::string::npos : j - i);

		if (!w.empty() && n.find(" " + w) != std::string::npos)
			return true;
		if (j == std::string::npos)
			break;
		i = j + 1;
	}
	return false;
}

// Where an object found by the search is: a node's position, or the middle
// of a way or area.
static bool object_coord(const osmscout::Database &db, const osmscout::ObjectFileRef &ref,
			 osmscout::GeoCoord &c)
{
	switch (ref.GetType()) {
	case osmscout::refNode: {
		osmscout::NodeRef n;

		if (!db.GetNodeByOffset(ref.GetFileOffset(), n))
			return false;
		c = n->GetCoords();
		return true;
	}
	case osmscout::refWay: {
		osmscout::WayRef w;

		return db.GetWayByOffset(ref.GetFileOffset(), w) && w->GetCenter(c);
	}
	case osmscout::refArea: {
		osmscout::AreaRef a;

		return db.GetAreaByOffset(ref.GetFileOffset(), a) && a->GetCenter(c);
	}
	default:
		return false;
	}
}

extern "C" int hbas_map_search(struct hbas_map *m, const char *query, double near_lat,
			       double near_lon, struct hbas_search_result *out, int max)
{
	if (!m || !query || !out || max <= 0)
		return -1;
	try {
		if (!m->locations)
			m->locations = std::make_shared<osmscout::LocationService>(m->database);

		std::string q = expand_abbreviations(query);
		osmscout::LocationStringSearchParameter param(q);

		// a street with no town in the query: look in the bike's region
		if (m->towns.empty()) {
			struct visitor : osmscout::AdminRegionVisitor {
				hbas_map *m;
				Action Visit(const osmscout::AdminRegion &r) override
				{
					auto ref = std::make_shared<osmscout::AdminRegion>(r);

					for (const auto &a : r.aliases) {
						osmscout::NodeRef n;

						if (m->database->GetNodeByOffset(a.objectOffset, n))
							m->towns.emplace_back(n->GetCoords(), ref);
					}
					return visitChildren;
				}
			} v;
			v.m = m;
			m->locations->VisitAdminRegions(v);
		}
		{
			double best = 1e18;
			osmscout::GeoCoord here(near_lat, near_lon);

			for (const auto &[c, r] : m->towns) {
				double d = osmscout::GetEllipsoidalDistance(here, c).AsMeter();

				if (d < best) {
					best = d;
					param.SetDefaultAdminRegion(r);
				}
			}
		}
		osmscout::LocationSearchResult result;

		param.SetSearchForLocation(true);
		param.SetSearchForPOI(true);
		param.SetPartialMatch(true);
		param.SetStringMatcherFactory(std::make_shared<osmscout::StringMatcherTransliterateFactory>());
		param.SetLimit(200);
		if (!m->locations->SearchForLocationByString(param, result))
			return -1;
		// the index splits several words into town / street guesses and
		// can miss the obvious street ("west canal"): search the longest
		// word alone too; the ranking below sorts it out
		{
			std::string longest, w;

			for (size_t i = 0; i <= q.size(); i++) {
				if (i == q.size() || q[i] == ' ') {
					if (w.size() > longest.size() && !std::isdigit((unsigned char)w[0]))
						longest = w;
					w.clear();
				} else {
					w += q[i];
				}
			}
			if (!longest.empty() && longest != q) {
				osmscout::LocationStringSearchParameter p2(longest);
				osmscout::LocationSearchResult r2;

				p2.SetSearchForLocation(true);
				p2.SetSearchForPOI(true);
				p2.SetPartialMatch(true);
				p2.SetStringMatcherFactory(param.GetStringMatcherFactory());
				p2.SetDefaultAdminRegion(param.GetDefaultAdminRegion());
				p2.SetLimit(200);
				if (m->locations->SearchForLocationByString(p2, r2))
					result.results.splice(result.results.end(), r2.results);
			}
		}

		struct hit {
			int words;      // query words found in the label
			bool town;
			int quality;
			double dist;
			hbas_search_result r;
		};
		std::vector<hit> hits;
		osmscout::GeoCoord near(near_lat, near_lon);

		for (const auto &e : result.results) {
			std::string label;
			osmscout::ObjectFileRef obj;
			int quality;

			if (e.address && e.location) {
				label = e.location->name + " " + e.address->name;
				obj = e.address->object;
				quality = e.addressMatchQuality;
			} else if (e.poi) {
				label = e.poi->name;
				obj = e.poi->object;
				quality = e.poiMatchQuality;
			} else if (e.location && !e.location->objects.empty()) {
				label = e.location->name;
				obj = e.location->objects.front();
				quality = e.locationMatchQuality;
			} else if (e.adminRegion) {
				// a town is usually a name attached to its county
				// (an "alias"): list the towns that match
				for (const auto &a : e.adminRegion->aliases) {
					osmscout::NodeRef n;
					hit h{};

					if (!words_start_with(a.name, q) ||
					    !m->database->GetNodeByOffset(a.objectOffset, n))
						continue;
					h.quality = e.adminRegionMatchQuality;
					h.town = true;
					h.dist = osmscout::GetEllipsoidalDistance(near, n->GetCoords()).AsMeter();
					std::snprintf(h.r.label, sizeof(h.r.label), "%s, %s", a.name.c_str(),
						      e.adminRegion->name.c_str());
					h.r.lat = n->GetCoords().GetLat();
					h.r.lon = n->GetCoords().GetLon();
					if (std::none_of(hits.begin(), hits.end(), [&](const hit &o) {
						    return !std::strcmp(o.r.label, h.r.label);
					    }))
						hits.push_back(h);
				}
				if (!words_start_with(e.adminRegion->name, q))
					continue;
				label = e.adminRegion->name;
				obj = e.adminRegion->object;
				quality = e.adminRegionMatchQuality;
			} else {
				continue;
			}
			if (e.adminRegion && label != e.adminRegion->name)
				label += ", " + e.adminRegion->name;

			osmscout::GeoCoord c;
			if (!object_coord(*m->database, obj, c))
				continue;
			// the same street is often several ways: keep one per label
			if (std::any_of(hits.begin(), hits.end(), [&](const hit &h) {
				    return label == h.r.label;
			    }))
				continue;
			hit h{};
			h.quality = quality;
			h.dist = osmscout::GetEllipsoidalDistance(near, c).AsMeter();
			std::snprintf(h.r.label, sizeof(h.r.label), "%s", label.c_str());
			h.r.lat = c.GetLat();
			h.r.lon = c.GetLon();
			hits.push_back(h);
		}
		// most typed words matched first, then exact before partial, then nearest
		for (auto &h : hits)
			h.words = count_matching_words(h.r.label, q);
		std::stable_sort(hits.begin(), hits.end(), [](const hit &a, const hit &b) {
			if (a.words != b.words)
				return a.words > b.words;
			if (a.town != b.town)
				return a.town;          // "madi": Madison before Madison Street
			return a.quality != b.quality ? a.quality < b.quality : a.dist < b.dist;
		});
		int n = 0;
		for (const auto &h : hits) {
			if (n == max)
				break;
			out[n++] = h.r;
		}
		return n;
	} catch (const std::exception &) {
		return -1;
	}
}
