# Geozones (ED-269 / ED-318)

_QGroundControl_ can load the UAS geographical zones (geozones) published by aviation authorities, show them on the maps, check planned missions against them and alert you when a vehicle approaches or enters them.

::: warning
These checks are advisory. Always check the official sources and obtain any required authorisation before flying.
:::

## Overview

The following data is supported (see the [format support matrix](#format-support-matrix) for the details of each format):

- EUROCAE ED-269 (JSON) and ED-318 (GeoJSON) geozone files, feeds and services.
- AIXM 5.1 airspaces, like the UK flight restriction zones published by NATS.
- The temporary flight restrictions (TFRs) published by the FAA.
- The zones of the German DiPUL and French IGN map services.

The zones are shown on the map in both the Plan and Fly views, colored by restriction: prohibited (red), authorisation required (orange), conditional (yellow), no restriction (green) and U-space airspace (blue).
Zones whose published restriction isn't recognised are shown as unknown (gray), and are checked and alerted like conditional zones.

Where the zones are used:

- **Plan View**: the [mission check](#mission-check) warns when the planned flight enters zones, suggests [routes around them](#route-around-zones), exports a [pre-flight briefing](#pre-flight-briefing), records the data the plan was checked against ([plan data audit](#plan-data-audit)) and [adds zones to the GeoFence](#add-geozones-to-the-geofence).
- **Fly View**: [flight alerts](#flight) warn when a connected vehicle approaches or enters a zone, and guided commands are checked before they are sent.
- **Application Settings > Geozones**: the sources, authorisations and [settings](#settings-reference).

When zones are loaded, the **Geozones** button in the Plan View tool strip and the geozone indicator in the Fly View toolbar show a color legend, the number of loaded zones and sources, and any outdated data warning. The Plan View panel also has a **Show geozones** switch, and the expanded Fly View indicator has the display and flight alert settings. Both have **Manage Geozones...** to open the geozone settings.

## Loading Data {#loading-data}

### Files and Feeds {#files-and-feeds}

To add geozones:

1. Open _Application Settings > Geozones_, or press **Manage Geozones...** in the Plan View **Geozones** panel or the Fly View geozone indicator.
1. Press **Add File...** and select a geozone file (`.json`, `.geojson`, AIXM `.xml` or a `.zip` package), or press **Add Feed...**, select a national feed (or **Other URL** to enter an `https://` address) and press **Download**, or press **Add Service...** for a service needing credentials or an area (see [Geozone Services](#geozone-services)).

The national feeds offered by **Add Feed...** are **Finland (Traficom)**, **Latvia (LGS)**, **Luxembourg (Geoportail)**, **Sweden (LFV)**, **United States TFRs (FAA)**, **Switzerland (FOCA)**, **Germany aviation zones (DiPUL)** and **United Kingdom FRZ (NATS)**.
**Germany aviation zones (DiPUL)** holds the control zones, airports, aerodromes and flight restriction areas of the German DiPUL service (licensed CC BY-ND 4.0), and **United Kingdom FRZ (NATS)** the flight restriction and runway protection zones of protected aerodromes and the danger areas from the AIXM dataset NATS publishes every AIRAC cycle.
German zone types are mapped to restrictions: restricted areas and temporary no-fly zones are prohibited, aviation, sensitive (including hospitals) and nature zones require authorisation, and residential areas, roads, railways and waterways are conditional.

Several files and feeds can be loaded at the same time. Files and downloads larger than 100 MB are rejected. Zip archives are read directly; when a package holds several geozone files (`.json`, `.geojson` or AIXM `.xml`), they are loaded together as one source, zones published in more than one file are kept once, and files of another format, like metadata, are left out. When a server answers with an error document or a web page instead of zones, its message is shown. Loading the same file again (from the same folder), or a feed with the same URL, replaces its zones; files with the same name in different folders are separate sources.
Files are loaded in the background, and the Plan View map is moved to the new zones if they are not in view.

_QGroundControl_ keeps a copy of each source and reloads them automatically on startup. Feeds are checked for updates when they are older than 24 hours (15 minutes for TFRs) by default, or sooner when the server tells how long its data stays current (HTTP `Cache-Control: max-age` or `Expires`) and that is shorter, though not more often than every 5 minutes. **Refresh Feeds** checks them immediately. Feeds which have not changed on the server are not downloaded again and keep the date of their data, and a feed republished with only new dates or a new title keeps its zones. If a download fails, the previous data is kept and the error, such as an untrusted certificate or a server which didn't respond within 2 minutes, is shown below the source. A failed feed is tried again after 5 minutes, then twice as long after each further failure, up to 6 hours or its refresh interval; **Refresh Feeds** tries at once.
If a new copy can't be saved, for example because the disk is full, the previous copy is kept and a message is shown. If the list of stored sources can't be read, the stored copies are moved to a `backup-<date>` folder in the `GeoZones` folder of the application data location so they can be added again; a list saved by a newer version of _QGroundControl_ is left untouched.

The **United States TFRs (FAA)** feed lists the current temporary flight restrictions. The details of each NOTAM are downloaded with the list for its effective times and altitudes, and kept for offline use; only new or changed NOTAMs are downloaded again. A TFR whose details can't be downloaded is treated as prohibited at all times and altitudes. Where a NOTAM has several areas, its TFRs span the altitudes of all of them. Check the NOTAM for exceptions.

European NOTAMs can't be loaded, since EUROCONTROL only provides them to organisations which signed its data user agreement.

### Zones of the Map Area {#zones-of-the-map-area}

Some services serve their zones by area. In the Plan View **Geozones** panel, **Download zones for the map area** downloads the zones of the map area, up to about 100 km wide, from **Germany, all zones (DiPUL)** (including residential areas, hospitals and nature reserves) or **France, open category (IGN)** (where flights are prohibited or limited to a maximum height). The provider covering the map is chosen. Downloaded areas are kept for offline use and refreshed like feeds. When the server limits its responses, the further pages are downloaded too (up to 50), and a warning is shown below the source if zones are still missing.

### Geozone Services {#geozone-services}

**Add Service...** adds a feed from a service which needs credentials, an ED-318 query or the area of interest. Pick a service, or **Other service**, check its address and settings, enter its secret if it needs one and press **Download**:

- **ED-318 geozone service**: a service following the retrieval interface of EUROCAE ED-318, which is sent the area and time of interest in a POST request (`geometry`, `startDateTime`, `endDateTime`) and answers with the zones in `uasZones`. Enter the address of its UAS zones endpoint.
- **Germany ED-318 service (DFS)**: the ED-318 interface of the DFS Geospatial Information Service (`https://dipul-service.dfs.de/api/geofencing/v2/uas-zones`), with the user and password of a dipul account, which DFS grants on request through the dipul contact form. It answers areas up to 100 km², so the margin around the mission and the vehicle is reduced to fit, and the zones of a larger mission area are not requested.
- **Zones of the area (GET)**: a service taking the area in its address. Write `{bbox}` (west,south,east,north), `{west}`, `{south}`, `{east}`, `{north}`, `{lat}`, `{lon}` (the center), `{radiusM}`, `{radiusKm}`, `{radiusNm}`, `{start}` or `{end}` (ISO 8601 UTC) where the service expects them, for example `https://example.org/zones?bbox={bbox}`.
- **United States NOTAMs (FAA NMS)**: configuration only. The FAA NOTAM Management Service (`https://api-nms.aim.faa.gov`) publishes NOTAMs as GeoJSON or AIXM 5.1 events to clients registered with `NOTAMS@faa.gov`, using OAuth2 client credentials, but _QGroundControl_ does not read NOTAMs yet. FAA TFRs are available without credentials from the **United States TFRs (FAA)** feed.

The authentication can be an API key in a header or a query parameter, a bearer token, a user and password (HTTP basic authentication) or OAuth2 client credentials, for which _QGroundControl_ requests a token from the token URL with the client id and secret and requests a new one when it expires or is refused. **Zones member** names the member of a JSON response holding the zones, like `data.geojson`, when the service wraps them; ED-318 replies are unwrapped automatically. Services, like other feeds, are refreshed periodically and kept for offline use.

Secrets (API keys, tokens, passwords and client secrets) are kept unencrypted in the _QGroundControl_ settings, since it has no secure store on all platforms. They are never written to the list of sources, the logs, the provenance or exported packs, and are only sent to the server of the service, not after a redirect to another server. Removing a source removes its secret. A source whose secret is missing, like after importing a pack, says so and has a field to enter it.

### Area of Interest {#area-of-interest}

With **Download the zones around the mission and the vehicle**, a service is requested for the area of interest: the bounds of the planned mission and the position of the active vehicle, with **Service download margin** (5 km by default) around them. The source shows the area downloaded. When the mission or the vehicle leaves that area, the zones of the new area are downloaded, unless **Download services around the mission and vehicle** is turned off in the settings; refreshes keep the area while it still covers the mission and the vehicle. A service with an area is only added once a mission is planned or a vehicle is connected.

### Integrity Checks {#integrity-checks}

If a server sends the SHA-256 of its data in a `Repr-Digest`, `Content-Digest` or `Digest` header, or a service publishes it in a file whose address is given in **SHA-256 URL** (like `{url}.sha256`, in the format `sha256sum` writes), the data is checked against it (the headers of compressed responses are not checked, since they give the SHA-256 of the compressed data). Verified sources say so. Data which doesn't match, or whose SHA-256 can't be downloaded, isn't loaded: the previous data is kept and the source shows that the integrity check failed. Only SHA-256 hashes are checked, not signatures.

### Managing Sources {#managing-sources}

Each source shows its format, zone count and date, for feeds when the server last confirmed the data current (**checked**), and after an update which changed zones, the zones added, removed and changed. A vehicle near or in a zone whose data changed is alerted again, since its limits or schedule may differ. A warning is shown if the data has expired (ED-318 `validTo`), is not valid yet, or is more than 28 days old by default (2 hours for TFRs, based on the ED-318 or FAA issue time, or else the file date or when the server last confirmed the downloaded data). Outdated sources are also reported once when _QGroundControl_ starts and in the Plan View warning, since they can miss zones.

Use **Remove** to remove a source and **Remove All** to remove all of them.
The check box of a source turns it off or on, in the settings and, with several sources, in the Plan View **Geozones** panel and the Fly View indicator. The zones of a source turned off are not shown, checked or alerted, its outdated data isn't reported, and it stays off after a restart until it is turned on or added again.

Problems found while reading a source are shown below it and in the details of the zones concerned: altitude limits in an unknown unit are ignored, so the zone applies at all altitudes, unknown days in a schedule are taken as every day, and times of day without UTC offset are taken as UTC when the time zone of the zone's country isn't known. Zone shapes are checked too: repeated points are dropped and boundaries need not be closed, but polygons with fewer than 3 distinct points or without area, polygons touching or enclosing a pole (within about 11 km of it) and circles without a positive radius are ignored, with a warning naming them. A zone left without any shape is counted as skipped. An invalid hole is ignored, so the zone covers it. A polygon whose boundary crosses itself is kept with a warning; the zone conservatively covers all the parts it encloses, also those it encloses twice (like the center of a star drawn in one stroke). Points on a zone boundary are inside the zone.

### Provenance and Offline Packs {#provenance-and-offline-packs}

Below each source, _QGroundControl_ shows where and when its data was obtained: the server which answered after redirects, its ETag or modification date, the beginning of the SHA-256 of the data received and whether it was verified or imported from a pack. **Export Provenance...** saves the provenance of all sources as JSON for briefings: per source its name, format, zone count, dates, service (without user or secrets) and provenance, which holds the provider, the address requested and the one which answered with the redirects, the validators, the time of the download, the SHA-256 of the data received, the BLAKE2b-256 hash and size of the stored copy, the area downloaded and the integrity check.

To use the zones on a computer without network access, press **Export Pack...** to save the sources turned on in a zip file, then **Import Pack...** on the other computer. A pack holds the stored copy of each source in `sources/` and `manifest.json` with the provenance of the sources, their dates, validators, areas and services, without secrets. Imported sources replace the sources with the same address or file, keep the dates and provenance of their data, marked as imported from the pack, and are refreshed like other sources when the network is available. Data in the pack which doesn't match the hash of its provenance is left out.

## Format Support Matrix {#format-support-matrix}

Formats are recognised from the content, not the file name. The table summarises what _QGroundControl_ reads from each format; anything not listed is ignored.

| Format | Files / feeds | Geometry | Edges between published points | Altitude references | Schedules | Authorities / messages | Notes |
| ------ | ------------- | -------- | ------------------------------ | ------------------- | --------- | ---------------------- | ----- |
| **ED-269** (JSON) | Files and feeds, also zipped | Polygons with holes, multipolygons, circles (center and radius); no arcs | Straight in latitude and longitude | `AGL`/`SFC`, `AMSL`/`MSL`, `WGS84`, `STD` (flight level, with the QNH); limits in m or ft (`uomDimensions`, which also applies to circle radii) | `applicability`: permanent, start/end date-times, daily times by day of the week in UTC, with their UTC offset or without, then in the [local time](#map-display) of the country or UTC, sunrise, sunset and civil twilight | Zone authority (name, service, contact, email, phone, site, purpose, notice), message, reasons, conditions, exemptions, U-space class, extended properties | Restrictions `PROHIBITED`, `REQ_AUTHORISATION`, `CONDITIONAL`, `NO_RESTRICTION`, `USPACE`; others are unknown |
| **ED-318** (GeoJSON) | Files and feeds, also zipped; ED-318 services | Polygons with holes, multipolygons, geometry collections, circles (`Point` with a `Circle` extent, radius in m); no arcs | Straight in latitude and longitude | As ED-269, per geometry `layer` (m or ft for the limits only) | `limitedApplicability`, as ED-269 | As ED-269; localized texts in your language when published | Dataset `metadata` (provider, issue date, `validFrom`, `validTo`) used for outdated data warnings |
| **AIXM 5.1** (XML) | Feeds, also zipped (like the NATS AIRAC dataset) | Outer boundaries of geodesic strings, line strings and arcs by center point, circles by center point; holes are not read | `GeodesicString`: great circle. `LineStringSegment`: straight in latitude and longitude. Arcs: circles of the published radius | `SFC`, `MSL`, `STD`, `W84` (WGS84); limits in ft, flight levels or m; `GND` is 0, `UNL` has no ceiling | Not read: zones are always active | Authority "UK CAA / NATS" for UK designators (`EG...`); message for flight restriction zones and danger areas | Type `P` is prohibited, `R` and the local types `FRZ`/`RPZ` require authorisation, others (like `D`) are conditional |
| **FAA TFR** (GeoJSON) | **United States TFRs (FAA)** feed, with the NOTAM details | Polygons; circles from the NOTAM details replace the 36-sided polygons approximating them | Great circle when the NOTAM details code the boundary points as great circle points (`GRC`), straight otherwise | From the NOTAM details: height (`AGL`), altitude (`AMSL`) or flight level, in ft; all altitudes without details | NOTAM effective and expiry times and schedules (UTC); always active without details | Authority FAA, message with the NOTAM number, TFR type (`LEGAL`) as reason | Always prohibited; live data refreshed every 15 minutes and outdated after 2 hours by default |
| **DiPUL** (GeoJSON, Germany) | **Germany aviation zones (DiPUL)** feed, **Germany, all zones (DiPUL)** map areas | Polygons, multipolygons | Straight in latitude and longitude | Floor in m `AGL` or ft `MSL`, ceiling in ft `MSL` or flight level; temporary zones with their own unit (m, ft, FL) and reference | Start and end times of temporary zones | Authority "DFS / BMDV (DiPUL)", legal basis as message, zone type as reason | Restriction from the zone type, unknown types conditional; English names unless _QGroundControl_ is in German |
| **IGN UAS** (GeoJSON, France) | **France, open category (IGN)** map areas | Polygons, multipolygons | Straight in latitude and longitude | Maximum height in m `AGL` (prohibited above it), or all altitudes | Not published: zones are always active | Authority "DGAC / IGN", the remark as conditions | All zones are prohibited, entirely or above their maximum height; temporary restrictions are published by NOTAM and not included |

How a zone boundary runs between two published points depends on the format. For long edges, especially at high latitudes, a great circle (geodesic) and a line straight in latitude and longitude can be several hundred meters apart:

- **ED-269, ED-318, DiPUL and IGN GeoJSON**: straight in latitude and longitude, as GeoJSON defines its lines (RFC 7946, section 3.1.1). ED-269 polygons are GeoJSON polygons and ED-318 uses the GeoJSON geometries; neither standard defines its edges otherwise.
- **AIXM 5.1**: `GeodesicString` segments are great circles. `LineStringSegment` segments in EPSG:4326 are straight in latitude and longitude, which the AIXM coding guidelines use for boundaries along parallels. Arcs (`ArcByCenterPoint`) are circles of the published radius around their center, with their angles taken as bearings.
- **FAA TFRs**: the NOTAM details code the boundary points of each TFR as great circle points (`GRC`), so its edges are great circles when the details are available, and straight otherwise. Circular TFRs are published as 36-sided polygons, whose sides cut slightly inside the circle (by about 7 m for a radius of 1 NM); when the NOTAM details give the circle (its center and radius), the zone is that circle instead. Polygons approximating circles in other sources are used as published.

Edges are checked as straight lines in latitude and longitude, so when zones are loaded, great circle edges get extra points on their great circle (up to 1023 per edge) and arcs a point every few degrees, keeping the boundary used by the checks, maps and fences within 1 m of the published one. On maps, long straight edges get extra points too, since the map projection would otherwise move them.

Vehicles fly the legs of a path along great circles, so the checks follow them too: legs whose great circle is more than 1 m from the line straight in latitude and longitude, like a 56 km leg along the 60th parallel which passes about 105 m north of it halfway, are checked in pieces along the great circle. Along paths whose terrain the checks look up, like guided commands, returns and alerts, the terrain is taken every 100 m (250 m for returns), for up to 1000 positions per path; on longer paths the positions are spread farther apart, which is reported as approximate (see [Approximate Results](#approximate-results)).

Altitude limits relative to the WGS84 ellipsoid are converted using the EGM96 geoid, within a few meters.

Limits published as flight levels (pressure altitude, `STD`) are approximate: their height depends on the air pressure and temperature. They are converted to heights above mean sea level with the **QNH** setting, as an altimeter set to it would read them in the ICAO standard atmosphere: the pressure altitude less 44330.8 m × (1 − (QNH / 1013.25 hPa)^0.190263), about 8.3 m lower per hPa below 1013.25 hPa. The zone details show both, like "FL 95 ≈ 2810 m AMSL (QNH 1003 hPa)". Since flight levels are lower in air colder than standard (by about 4 % of their height per 10 °C), the checks, alerts, altitude profile and strip lower floors and raise ceilings given as flight levels by 4 % of their height. Set the local QNH before planning or flying near such zones; a change applies at once to the checks and alerts.

## Map Display {#map-display}

Zones that are not active at the current time are drawn with a fainter fill. Schedules based on sunrise, sunset or civil twilight are computed for the location of each zone. Daily times published in UTC (`08:00Z`) or with an offset from UTC (`08:00+01:00`) keep it all year. Since the formats don't tell whether times without offset (`08:00`) are UTC or local times, to be safe such a zone counts as active whenever it is in either: in the local time of the zone's country, which follows its daylight saving time, or in UTC. So a zone active from 08:00 to 18:00 in Zurich counts as active from 07:00 to 18:00 UTC in winter and from 06:00 to 18:00 UTC in summer, for the map, the flight alerts, the mission check, the routes and the briefing. The local time of European countries with one time zone on their mainland is known, with those of the Canary Islands, Madeira and the Azores; times of zones in other countries or without country are taken as UTC only, with a warning. The details of the zone show such times in local time with the assumed time zone, like "Every day 08:00-18:00 Europe/Zurich (time zone assumed)".

**Show geozones** in the Plan View **Geozones** panel (or **Show geozones on maps** in the expanded Fly View indicator and the settings) hides or shows the zones.
The check boxes of the legend hide or show the zones of each restriction on the maps, for example to see the map under large U-space airspace. Hidden zones are not picked by map clicks, but the mission check, flight alerts and guided command checks still apply to them.

Click a zone on the map to see its details (restriction, altitude limits in your units, applicability schedule, reason, message, responsible authority, exemptions, U-space class, other published properties and the source it was loaded from). Where zones overlap, choose the zone at the top of the details. In the Plan View this works when the click doesn't add a waypoint or rally point or select a fence. In the Fly View, when guided actions are available, the map click menu has **Geozones here**. When the map is zoomed in, each zone also shows a label which opens its details.

To find a zone, type part of its name or identifier in the search field of the Plan View **Geozones** panel or the Fly View geozone indicator, then pick a result to move the map to the zone and show its details.
When many small zones are in view, the smallest are left out until you zoom in, and the Plan View **Geozones** panel shows how many are hidden.

## Planning {#planning}

### Mission Check {#mission-check}

When the planned mission path or the area of a survey, corridor scan or structure scan enters a prohibited, authorisation required or conditional zone, a warning listing the zones is shown at the top of the Plan View map and the zones are outlined in white. Click the warning to show the zones on the map.
Besides the drawn flight path, the check follows loiter circles with a radius, the loiter of landing patterns, the distance VTOL transitions fly on past their waypoint (**VTOL TransitionDistance** in _Application Settings > Plan View_), the loops of **DO_JUMP** items, and the return the vehicle flies after a **Return To Launch** item (see [Return and Contingency Paths](#return-and-contingency-paths)). The warning names what enters zones the drawn flight path doesn't, like "Only the loiter at item 3 enters ..." or "Only the jump at item 8 enters ...".
A **DO_JUMP** flies the items from its target to the jump again as many times as its repeat count, including the straight leg back to the target, so zone schedules are checked at the times the repeats are flown. A jump repeating without end is flown once more and named "repeated without end"; very long repeats are cut off after about 5000 legs. A jump past a **Return To Launch** skips it: the check follows the items after it, whose legs the Plan View doesn't draw, as part of the jump, and only the return the vehicle reaches is flown.
U-space airspaces the mission enters are listed too, since flying there requires a flight authorisation from a U-space service provider. Zones without restriction are listed for their conditions, they never hide restricted zones.
Uploading a mission asks for confirmation, listing the reasons, when it enters prohibited zones you have no authorisation for (naming those the GeoFence doesn't exclude) or when the geozone data changed since the plan was saved and you didn't acknowledge the changes (see [Plan Data Audit](#plan-data-audit)). **Cancel** leaves the mission on the vehicle as it is.

The check compares the planned altitudes with the zone altitude limits, using the terrain elevation along the path for limits above ground. Between waypoints the altitude is taken to change evenly, so a leg which descends below a zone's ceiling only after passing it doesn't enter the zone. When the terrain or altitude is not known, the zone is assumed to apply: the terrain isn't interpolated across stretches without terrain data, so a zone with limits above ground counts as entered wherever the path passes over such a stretch.

The altitude profile below the Plan View map shades the restricted zones and U-space airspaces along the path between their floors and ceilings, so you can see where to climb or descend. Limits above ground follow the terrain, and limits that are unknown or beyond the chart extend to its edges.

If the vehicle supports a GeoFence, the warning also names the prohibited zones on the path which no exclusion fence covers, and **Add Exclusion Fences** adds them to the GeoFence so the vehicle stops at them. Fences added from a zone cover it whatever their simplification, while a fence which leaves out part of a zone, or a zone without area, doesn't count.

### Approximate Results {#approximate-results}

Where data is missing or a limit can only be estimated, the checks stay conservative, so the zone counts as entered, and say so: the mission check adds a line like "Approximate: Terrain unknown along part of the path, limits above ground taken as reached." to its warning, and the briefing lists the notes per zone and for the whole mission. The notes are:

| Note | JSON code | Meaning |
| ---- | --------- | ------- |
| Terrain unknown | `terrain_unknown` | Limits above ground were compared where the terrain elevation isn't known, so they were taken as reached. |
| Terrain sampled coarsely | `terrain_coarse` | The path is too long for the terrain positions (1000), which were spread farther apart than every 100 m. |
| Leg too long to follow its great circle | `great_circle_deviation` | A leg, like one across half the world, can't be checked within 1 m of its great circle. |
| Flight level limits | `flight_level` | Flight levels were converted with the QNH setting and widened by 4 %. |
| Zone schedule not fully known | `schedule_assumed` | The zone applies by a schedule which can't be fully evaluated, like one from an unknown daylight event, and was taken as active. |
| Altitude or its reference unknown | `altitude_reference_unknown` | A limit is in an unknown reference, or the altitude flown is unknown (like altitudes above home without the home altitude), so the limit was taken as reached. |

### Flight Time and Schedule Tolerance {#flight-time}

By default zone schedules are ignored, since the mission may be flown at another time. To check a flight at a known time, turn on **Check zones at the flight time** in the Plan View **Geozones** panel and enter the local start time (**Now** sets the current time). The mission check, the altitude profile and **Route Around Zones...** then leave out zones which aren't active while the mission flies through them, estimating when it does from its duration as if the mission is flown at an even pace. Since the take-off may be early or late and the pace may differ, a zone still counts when it is active within 15 minutes before or after the estimated times it is flown through. The warning shows the time and how many zones were left out.
The flight time belongs to the plan: it is saved with it, a plan opened without one has none, and a new plan, **Clear**, or a plan downloaded from the vehicle starts without one.

### Authorisations {#authorisations}

If you hold a permit for a zone, press **Add Authorisation...** in its details and enter the permit number and, optionally, its first and last days. While the authorisation is valid, the zone is listed as authorised instead of warned about by the mission check, and the flight alerts, guided command checks and **Route Around Zones...** leave it out. With **Check zones at the flight time**, the authorisation must cover the whole flight. Authorisations apply to the zone in every source, are listed in the **Authorisations** section of _Application Settings > Geozones_ where they can be removed, and are kept after a restart.
Saved plans keep the flight time, the authorisations of the zones the mission enters and the version of the geozone data (see [Plan Data Audit](#plan-data-audit)). When a plan with authorisations you don't have is opened, the Plan View banner lists them with **Add** and **Ignore**, so permits from someone else's plan are only added when you choose to.

### Return and Contingency Paths {#return-and-contingency-paths}

The return paths follow the parameters of the connected vehicle, read again whenever they, the rally points, the breach return point or the planned home position change:

- **PX4**: `RTL_TYPE` chooses the destination: 0 the closest of home and the rally points, 1 the closest rally point or mission landing (home without them), 3 the closest of home, the rally points and the mission landing, 5 the closest rally point (landing in place without one), 6 like 0 since the battery state is unknown. Types 2 and 4 continue the mission to its landing (no other path) or fly it in reverse and then home from the first waypoint after the takeoff, since PX4 lands at home instead of flying back to the takeoff. A destination must be 2 m closer than the next best one, like PX4 requires. The vehicle climbs to `RTL_RETURN_ALT` above the destination (or returns at its altitude when higher), descends to `RTL_DESCEND_ALT` at the destination and lands, or loiters there when `RTL_LAND_DELAY` is negative; fixed wings circle at `NAV_LOITER_RAD`. Multicopters lower the return altitude within `RTL_MIN_DIST` of the destination to the cone of `RTL_CONE_ANG`, at least twice `NAV_ACC_RAD` above it. A mission landing is a `DO_LAND_START` followed by a landing: the vehicle flies to its first waypoint, then the landing is part of the mission.
- **ArduCopter**: home or the closest rally point (`RALLY_INCL_HOME`, `RALLY_LIMIT_KM`). The vehicle climbs to `RTL_ALT` above home, at least `RTL_CLIMB_MIN` above its altitude and never lower than it (`RTL_ALT` 0 returns at the current altitude), limited by `RTL_CONE_SLOPE` near the destination and by the altitude fence (`FENCE_ALT_MAX` less `FENCE_MARGIN` when enabled), then descends to `RTL_ALT_FINAL` above home or lands. Both the centimeter parameters until Copter 4.6 and the meter parameters `RTL_ALT_M`, `RTL_CLIMB_MIN_M` and `RTL_ALT_FINAL_M` of later versions are read.
- **ArduPlane**: home at `RTL_ALTITUDE` above home (`ALT_HOLD_RTL` in centimeters on older versions, -1 for the current altitude) or the closest rally point at its altitude, climbing at once when lower and descending along the way when higher, then circling at `RTL_RADIUS` (or `WP_LOITER_RAD`). With `RTL_AUTOLAND` 1 the plane then flies the landing sequence of the closest `DO_LAND_START`, with 2 it flies to it directly, with 4 it joins the closest waypoint after a `DO_RETURN_PATH_START`.
- Other vehicles return straight to the closest of home and the rally points at their return altitude parameter.

Until the parameters of a connected vehicle are loaded, and without a connected vehicle, the check assumes the defaults of the firmware and vehicle class of the plan (**Vehicle Info** in the Mission Start item without a connected vehicle): `RTL_RETURN_ALT` 30 m and `RTL_DESCEND_ALT` 10 m for PX4 multicopters, `RTL_TYPE` 1, `RTL_RETURN_ALT` 100 m and loitering for PX4 fixed wings, `RTL_TYPE` 1 and `RTL_RETURN_ALT` 60 m for PX4 VTOLs (the airframe defaults, which differ from the parameter defaults of 0, 60 m and 30 m), `RTL_ALT` 15 m with `RTL_CONE_SLOPE` 3 for ArduCopter and `RTL_ALTITUDE` 100 m for ArduPlane, without rally limits or fences beyond their defaults. The warnings say so, like "assuming the PX4 default return altitude of 30 m". For other firmware the return altitude is unknown, zones along the way count at any altitude and the warnings say "at an unknown altitude".

The check also follows the contingency paths: the return the vehicle would fly from each item of the mission and from points every 250 m or more between them (at most 100 starts), after a failsafe or a fence breach. With ArduPlane's `FENCE_ACTION` 6 or 7 (guided) it also follows the flight to the breach return point of the GeoFence (home without one, the closest rally point with `FENCE_RET_RALLY`) at `FENCE_RET_ALT`, between the fence altitudes, or at the return altitude. Fence actions which return to launch (PX4 `GF_ACTION` 3, ArduPilot `FENCE_ACTION` 1) are covered by the returns, other actions hold, brake, land or retrace the path flown (SmartRTL) and add no path. Restricted zones only contingency paths enter are listed apart and grouped by the items they would be flown from, like "A return to launch from items 3–7 would enter ...". Unlike the planned flight they don't ask for confirmation on upload.

The return paths are estimates: wind, the turns and climb rate of fixed wings, terrain following returns (`RTL_ALT_TYPE`), PX4's geofence avoidance, SmartRTL, QuadPlane returns (`Q_RTL_MODE`), VTOL approach loiters and ArduPilot's checks of rally points against the fence aren't modeled. PX4 VTOLs are taken to return in fixed wing mode, without the cone, landing at the destination.

### Route Around Zones {#route-around-zones}

**Route Around Zones...** in the warning suggests, for each leg between waypoints which enters restricted zones, the ways to avoid them and recommends the cheapest one:

- **Detour**: waypoints around the zones keeping 100 m from them, which **Insert** adds to the mission at the altitudes the leg changes evenly through. The detour is the shortest path around the outlines of the zones, so it passes between zones, enters the bays of concave zones which are wider than 200 m and stays out of narrower ones, and stays within holes of zones. Waypoints within 100 m of a zone leave it without coming much closer. Detours longer than three times the leg, or 3 km longer when that is more, aren't suggested.
- **Climb**: raising the waypoints of the leg to 10 m above the ceilings of the zones, like "climb to 160 m above home over Zone X", when the leg then stays within the maximum height above the terrain set in the dialog (120 m by default).
- **Descent**: lowering the waypoints of the leg to 10 m below the floors of zones which don't reach the ground, as long as the leg stays at least 10 m above the terrain.

The cost of a detour is the distance it adds; a climb or a descent counts 8 m of distance per meter the waypoints move, for climbing and descending back. Climbs and descents need the altitudes of the leg and the terrain along it, and **Apply** changes waypoints with relative or AMSL altitudes. Each way is checked like the mission, at the altitudes of the leg over the terrain where its data is loaded, and must not enter other restricted zones. When a way isn't possible the dialog tells why, like "No detour: item 3 is inside Zone X.", "No climb: Zone X has no ceiling in meters." or "No descent: Zone X extends to the ground."; other reasons are zones surrounding the leg, detours too long, climbs above the maximum height and descents too close to the terrain. Among very many zones close together, like hundreds of small zones forming a maze, the detour may only be close to the shortest, and the search of a leg stops after a few seconds of work with "No detour: the search for a way around Zone X reached its limit."; a waypoint placed in a gap between the zones splits the search into shorter ones. The routes are searched in the background while the dialog shows the progress, **Cancel** stops the search and **Search Again** restarts it. Changes to the mission or the zones start a new search. Check the new waypoints, altitudes, the terrain and obstacles before flying.

### Pre-flight Briefing {#pre-flight-briefing}

**Export Briefing...** in the Plan View **Geozones** panel or in the geozone banner above the map saves a briefing of the geozones of the planned mission. The file dialog proposes the folder and name of the open plan with `-briefing.pdf`; the extension of the file name chooses the format:

- **PDF** (`.pdf`): an A4 document to print or take along, with the mission name, the date and page numbers on each page.
- **Web page** (`.html`): a single file without external resources, which reads well on a phone and prints cleanly.
- **JSON** (`.json`): the same content for other tools, see below.

The briefing lists the mission (vehicle and firmware, planned start and estimated end, duration, distance, highest altitude, home and how the returns were modeled), then:

- **Zones the flight path enters**, in the order the mission enters them: where (after which item, how far along the mission, the estimated times with a planned start), the altitudes flown inside against the zone floor and ceiling at the entry, whether the result is approximate (see [Approximate Results](#approximate-results)), when the zone is active during the flight (widened by 15 minutes like the mission check) and its published schedule, the vertical limits as published, the authority and its contacts, the message and conditions, U-space class, exemptions, your authorisation with whether it covers the flight, and the source and content hash of the zone.
- **Zones only returns would enter**, with the returns and the items they would be flown from.
- **Restricted zones within 500 m** of the flight path, or passed above or below it, with their distance.
- **Data versions**: each source covering the mission with its format, file or feed, data (issue) date, the AIRAC cycle in effect at that date (not for live restrictions), its published validity, when it was last checked, and the hash of its content.

The JSON file has `"schema": "qgc-geozone-briefing"` and `"version": 1`. The version increases when fields are removed or change meaning, not when fields are added. Times are ISO 8601 UTC, distances and altitudes meters (altitudes above mean sea level), unknown values `null`. The fields are:

- `generated`, `generator`, `scheduleToleranceSeconds`, `nearbyDistanceMeters`, `approximations` (codes of the [approximate results](#approximate-results) of the mission and its returns).
- `mission`: `name`, `planFile`, `vehicle`, `firmware`, `plannedStart`, `plannedEnd`, `durationSeconds`, `distanceMeters`, `maxAltitudeAMSL`, `maxAltitudeAboveHome`, `home` (`lat`, `lon`, `altitudeAMSL`), `itemCount`, `returnAssumptions` (texts).
- `zones`: `involvement` (`entered`, `contingency` or `nearby`), `key`, `identifier`, `country`, `name`, `restriction` (`prohibited`, `req_authorisation`, `conditional`, `no_restriction`, `u_space` or `unknown`), `restrictionCode` (as published), `restrictionText`, `reasons`, `verticalLimits`, `schedule`, `activeDuringFlight`, `activePeriods` (`start`, `end`), `crossings` (`path`: empty for the flight path or what the vehicle does, like "the loiter at item 3", `entryItem` and `exitItem`: sequence number of the last item reached before, `entryDistance`, `exitDistance`, `entry` and `exit` (`lat`, `lon`), `entryTime`, `exitTime`, `pathAltitudeMin`, `pathAltitudeMax`, `zoneFloor`, `zoneCeiling`, `active`, `approximations`), `inArea`, `contingencies` (texts), `distance`, `passed` (`above`, `below` or `null`), `authorities` (texts), `message`, `conditions`, `otherReasonInfo`, `uSpaceClass`, `regulationExemption`, `authorisation` (`permit`, `validFrom`, `validTo`, `status`, `coversFlight`, or `null`), `source`, `contentHash` (hexadecimal) and `approximations` (codes).
- `sources`: `key`, `name`, `format`, `provider`, `dataDate`, `loaded`, `checked`, `validFrom`, `validTo`, `airacCycle`, `contentHash` (hexadecimal).

### Plan Data Audit {#plan-data-audit}

When a plan is saved while geozone data is loaded, the plan file keeps, in the `audit` object of its `geoZones` section, the version of each source covering the mission (data date and content hash) and the content hash of each zone the mission or a return from along it enters, whatever its schedule.
When the plan is opened again, and whenever the sources reload while it is open, the zones are compared with the current data. If they differ, the banner above the map warns, like "Geozone data changed since this plan was saved: 2 zone(s) modified, 1 new. Review the plan.":

- **modified**: a zone of the plan whose published content (geometry, limits, schedule, restriction, name...) changed. Zones are matched by their source, identifier, name and restriction, or by their country and identifier when they were renamed, reclassified or moved to another source.
- **removed**: a zone of the plan no source has anymore, or whose source was removed or disabled.
- **new**: a zone the mission now enters which came with updated or newly loaded data. Zones the mission enters only because you edited it don't count.
- **source updated** or **removed**: the data of a source covering the mission changed, even when the zones of the mission didn't.

**Details** lists the changes and **Acknowledge** hides the warning, and the confirmation it asks for on upload, until the data changes again. Saving the plan records the current data, so the warning disappears. Plans saved without this record, like older plans, open without warning.

### Add Geozones to the GeoFence {#add-geozones-to-the-geofence}

If the vehicle supports GeoFences, loaded zones can be converted into exclusion fences which fit into the vehicle's fence storage. Press **From Geozones...** below **Polygon Fence** and **Circular Fence** in the [GeoFence](../plan_view/plan_geofence.md) panel, then in the dialog:

1. Check the zone types to add (**Prohibited**, **Authorisation**, **Conditional**).
1. Leave **Only zones in current map view** checked to only add the zones visible on the map (recommended for national files that contain many zones).
1. Select when the zones must be **Active**: **Any time**, **Now**, or within the **Next 3 hours** or **Next 24 hours**.
1. Select how far fences may be simplified to fit (**Simplify up to**). Fences are only simplified as far as needed, the most important zones the least. Simplified fences always enclose the whole zone and extend up to twice the tolerance beyond it. Select **Off** to keep the published vertices.
1. Check the zones selected and those left out, then press **Apply** to add the fences to the plan, or **Upload to Vehicle**.

The dialog shows how much of the vehicle's fence the selection uses, like _Vehicle fence: 52 of 64 points_:

- **PX4** stores 64 fence items: one per polygon vertex, circle and breach return point.
- **ArduPilot** (4.0 and later) stores the fence in a storage area of the flight controller: each polygon takes 2 bytes plus 8 bytes per vertex, each circle 13 bytes and the return point 9 bytes, and a polygon has at most 255 vertices. On flight controllers with 16 KB of storage, like the Pixhawk and Cube, Copter (and Heli) has 560 bytes (70 items) and Plane, Rover and Sub have 672 bytes (84 items). Flight controllers with less storage hold fewer points; a rejected upload is reported. When `BRD_SD_FENCE` stores the fence on the SD card, its size is used instead. `FENCE_TOTAL` only counts the points of the last upload and is not a limit.
- Without a connected vehicle the fence is fitted to what both PX4 and ArduPilot copters store (64 points, 560 bytes) and circles are added as polygons enclosing them.

The panel warns when the vehicle won't enforce exclusion fences: ArduPilot with `FENCE_TYPE` lacking polygon fences or `FENCE_ENABLE` at 0, PX4 with `GF_ACTION` set to None.

Zones are selected in priority order until the fence is full: prohibited zones first, then other restricted zones (authorisation required, conditional and unknown), then the others. Within those, zones the planned flight enters (between their altitude limits) come first, then those within 1 km of the flight, then the farther ones by distance, then those with fewer fence points. Fences already added from geozones are replaced, while drawn fences and the breach return point are kept and take room. The left out zones are listed with the reason: no room left, too many points even simplified, the same area as another zone, or left out by you. Uncheck a zone to leave it out, which makes room for the next ones, or check a left out zone to include it.

Zones the flight only passes above or below are left out unless you check them, since their exclusion fence would stop the vehicle there.

**Upload to Vehicle** adds the selected fences to the plan and uploads the plan's fence (not the mission) like a plan upload, then reads the fence back from the vehicle and compares it with the plan: the number of polygons and circles, each vertex and circle center within 0.5 m, the circle radii and the breach return point. The dialog shows whether the vehicle stores the fence as planned, the differences, or why the vehicle rejected it, for example when it has no room for it.

The upload is aborted when the plan's fence is edited or another plan is opened (or the plan is saved under another name) before it is verified, since the vehicle wouldn't store the fence of the plan: the fence stays marked as changed, so upload it again. It fails when the vehicle disconnects meanwhile.

Fences added from geozones are named after their zones and listed separately in the GeoFence panel, where **Delete Geozone Fences** deletes them (see [Edit/Delete a GeoFence](../plan_view/plan_geofence.md#edit-delete-a-geofence)).

::: warning
GeoFences have no altitude limits, so a zone's vertical limits are not transferred and the whole zone is excluded at all altitudes.
Holes in zone polygons are also dropped. The ArduPilot fence storage is estimated, so verify the upload.
:::

## Flight {#flight}

### Flight Alerts {#flight-alerts}

While flying, _QGroundControl_ warns once (with a message and audio) when a connected vehicle comes within 100 m (or 30 m vertically, by default) of an active prohibited, authorisation required, conditional, unknown or U-space zone, or would enter it within 30 seconds at its current speed, direction and climb rate, and again when it enters the zone. Alerts based on the velocity give the time to entry, so fast vehicles are warned in time. The velocity is the one the vehicle sends with its position, or is estimated from its positions when it sends none. While a vehicle's link or position is lost, its alerts pause, since its last position isn't where it is, and the geozone indicator says so. Limits above ground use the vehicle's height above terrain, and the predicted path and guided commands use the terrain ahead where its data is loaded, so rising ground towards a zone counts. Where it isn't loaded, the terrain ahead is unknown rather than taken from the last known position, so zones with limits above ground count as reaching any altitude there. Without terrain data for the vehicle position, zones with limits above ground are assumed to apply at any altitude once the vehicle is armed; a disarmed vehicle is taken to be on the ground.

Every connected vehicle is monitored on its own, and alerts name the vehicle when several are connected. A vehicle which reconnects starts afresh, even with the same system id, so its alerts repeat. The geozone indicator in the Fly View toolbar turns red, orange, yellow or blue when the active vehicle is near or inside a zone, and is dimmed while no vehicle is monitored. Click it to see the nearby zones; expand it to change the display and flight alert settings.

### Guided Command Checks {#guided-command-checks}

Guided commands of the Fly View are checked too: the confirmation of **Go To Location**, **Orbit**, **Change Altitude**, **Takeoff**, **Return**, **Land**, setting the current mission item and starting, continuing or resuming the mission names the active restricted zones and U-space airspaces the vehicle would enter, including zones it is in which it would leave and enter again, updated as the altitude slider moves, and a go to location which enters zones is always confirmed. Starting the mission also checks the mission on the vehicle for a flight starting now and whether the vehicle's GeoFence excludes its prohibited zones.

- **Return** follows the return the vehicle flies from where it is, with its return parameters, to home, its rally points or the landing of the mission on it, or along the mission, like the [return paths](#return-and-contingency-paths) of the mission check, for example "The return to rally point 2 would enter geozones: ...". With **Smart RTL** checked the vehicle retraces the path it flew, which isn't checked.
- **Land** follows the descent to the ground below the vehicle (to the home altitude where the terrain isn't known), so zones from the ground up to a height below the vehicle count.
- Setting the current mission item follows the flight from the vehicle to the item and on along the mission, and resuming the mission the flight from the vehicle climbing to the altitude of the item it resumes at. An item without a coordinate counts from the item with a coordinate before it.

### Spoken Alerts, Repeats and Muting {#spoken-alerts}

Alerts are shown in a popup below the toolbar, the most restrictive zone first, and spoken. Alerts raised together, also by several vehicles, are spoken as one message naming the two most restrictive and counting the others ("..., and 2 more"). Spoken messages are at least 5 seconds apart, unless a more urgent alert comes: a more restrictive zone, or entering rather than approaching it. While a vehicle stays inside a prohibited zone, "Still inside geozone ..." is spoken and shown again every 30 seconds until the alert is acknowledged or the vehicle leaves the zone. **Acknowledge** in the popup acknowledges an alert, as do **Show** and **Acknowledge All** in the indicator. **Mute Alerts for 1 min** in the popup silences the spoken alerts for a minute (the indicator shows it, with **Unmute**); entering a prohibited zone is still announced once, and alerts raised while muted are not spoken later.

### Altitude Strip and Approach Path {#altitude-strip}

While zones are above or below the active vehicle, a strip on the left of the Fly View shows their floors and ceilings with the vehicle's altitude, above the ground when the terrain is known.

When the active vehicle would enter a zone within the approach alert time at its current velocity, the Fly View map draws its path to the entry point in the zone's color, with the zone name and the seconds left.

### Vehicle Fence Breaches {#vehicle-fence-breaches}

When a vehicle reports a breach of its own fence (MAVLink `FENCE_STATUS`), _QGroundControl_ shows it in the alert popup, the geozone indicator turns red and its page names the kind of breach (boundary, minimum or maximum altitude) and how many breaches the vehicle counted, until the vehicle reports the breach cleared.

### Alert Log {#alert-log}

Alerts are also appended to a `GeoZoneAlerts-<date>.csv` file per day (UTC) in the _Telemetry_ folder of the application load/save path, with the time, vehicle, position, altitude, terrain elevation, event, distance, zone and predicted time to entry, for reviewing flights.
The indicator lists the **Recent Alerts** of the session, newest first, followed by those logged earlier the same day. Alerts not looked at yet are shown in bold and counted in a red badge on the indicator; **Acknowledge All** clears them. **Show** opens the details of the alert's zone and marks the vehicle position at the time of the alert on the map for two minutes (click the mark to remove it).

## Settings Reference {#settings-reference}

Besides the **Sources** and **Authorisations**, the **Geozones** page of the _Application Settings_ has the following settings:

| Section | Setting | Default | Range | Description |
| ------- | ------- | ------- | ----- | ----------- |
| Display | **Show geozones on maps** | On | | Shows or hides the zones on the Plan and Fly View maps. The legend check boxes hide the zones of single restrictions. |
| Flight Alerts | **Flight alerts** | On | | Turns the flight alerts and the nearby zones of the toolbar indicator on or off. |
| Flight Alerts | **Speak flight alerts** | On | | Turns the audio of the alerts on or off. |
| Flight Alerts | **Log flight alerts** | On | | Appends the alerts to the [CSV log](#alert-log). |
| Flight Alerts | **Approach alert distance** | 100 m | 10–5000 m | How close horizontally the vehicle can come to a zone before it is alerted. |
| Flight Alerts | **Approach alert height margin** | 30 m | 0–1000 m | How close below the floor or above the ceiling of a zone the vehicle can come before it is alerted. |
| Flight Alerts | **Approach alert time** | 30 s | 0–600 s | How many seconds before entering a zone, at its current velocity, the vehicle is alerted. 0 turns these alerts off and alerts by distance only. |
| Altitude Limits | **QNH** | 1013.25 hPa | 850–1100 hPa | The local sea level pressure (altimeter setting) converting flight levels to heights above mean sea level. The default is the standard pressure of flight levels. |
| Data Updates | **Feed refresh interval** | 24 hours | 1–168 hours | How often feeds are downloaded again. |
| Data Updates | **Live restrictions refresh interval** | 15 min | 5–240 min | How often feeds of temporary flight restrictions (TFRs) are downloaded again. |
| Data Updates | **Outdated data warning** | 28 days | 1–365 days | The data age at which a source is reported as outdated. |
| Data Updates | **Outdated live restrictions warning** | 2 hours | 1–72 hours | The data age at which a TFR feed is reported as outdated. |
| Data Updates | **Download services around the mission and vehicle** | On | | Whether services taking an area download the zones of a new area when the mission or the vehicle leaves the one downloaded. |
| Data Updates | **Service download margin** | 5000 m | 500–50000 m | The distance around the mission and the vehicle downloaded from services taking an area. |

## Limitations {#limitations}

- The checks and alerts are advisory and depend on the data loaded: always check the official sources and obtain any required authorisation before flying.
- AIXM schedules are not read, so AIXM zones are always active, and holes of AIXM airspaces are not read.
- European NOTAMs and FAA NOTAMs from the NOTAM Management Service are not read. A TFR whose NOTAM details can't be downloaded is treated as prohibited at all times and altitudes; check the NOTAM for exceptions.
- Flight levels are converted with the QNH setting and a 4 % margin, and heights above the WGS84 ellipsoid with the EGM96 geoid, so these limits are approximate. Results relying on such limits, unknown terrain or altitudes, coarse terrain or schedules which can't be fully read are marked as [approximate](#approximate-results).
- Schedules with unknown days are taken as every day, which isn't marked as approximate; the zone details show the problem.
- Return and contingency paths are estimates, see [Return and Contingency Paths](#return-and-contingency-paths).
- GeoFences made from zones have no altitude limits and no holes, and the ArduPilot fence storage is estimated.
- Secrets of services are stored unencrypted in the settings, and only SHA-256 hashes are checked, not signatures.

Developers can find the architecture of the geozone module in the [Geozones developer guide](../../qgc-dev-guide/geozones.md).
