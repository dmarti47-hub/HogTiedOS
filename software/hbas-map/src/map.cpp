// SPDX-License-Identifier: MIT
// libosmscout behind the C interface in hbas/map.h. The sequence (open
// database, load style, project, load tiles, AGG painter) follows
// libosmscout's documented API and its DrawMapAgg demo.
#include "hbas/map.h"

#include <cmath>
#include <cstdio>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <osmscout/db/Database.h>
#include <osmscout/projection/MercatorProjection.h>
#include <osmscoutmap/MapService.h>
#include <osmscoutmap/StyleConfig.h>
#include <osmscoutmapagg/MapPainterAgg.h>

struct hbas_map {
	osmscout::DatabaseRef database;
	osmscout::MapServiceRef service;
	osmscout::StyleConfigRef style;
	osmscout::MapParameter parameter;
	osmscout::AreaSearchParameter search;
	osmscout::MercatorProjection projection;
	std::vector<unsigned char> rgb;          // AGG draws 24-bit RGB
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
