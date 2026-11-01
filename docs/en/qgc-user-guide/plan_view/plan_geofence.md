# Plan View - GeoFence

GeoFences allow you to create virtual regions within which the vehicle can fly, or in which it is _not allowed_ to fly.
You can also configure the action taken if you fly outside permitted areas.

![Geofence overview](../../../assets/plan/geofence/geofence_overview.jpg)

::: info
**ArduPilot users:** GeoFence support is only supported by Rover 3.6 and Copter 3.7 or higher. It also requires usage of a Daily build or Stable 3.6 (once available).
_QGroundControl_ will not display the GeoFence options if they are not supported by the connected vehicle.
:::

## Create a Geofence

To create a GeoFence:

1. Navigate to the Plan View
1. Select the _Geofence_ radio button above the Mission Command List

   ![Select geofence radio button](../../../assets/plan/geofence/geofence_select.jpg)

1. Insert a circular or polygon region by pressing the **Circular Fence** or **Polygon Fence** button, respectively.
   A new region will be added to the map and to the associated list of fences below the buttons.

:::tip
You can create multiple regions by pressing the buttons multiple times, allowing complex geofence definitions to be created.
:::

- Circular region:

  ![Circular Geofence](../../../assets/plan/geofence/geofence_circular.jpg)

  - Move the region by dragging the central dot on the map
  - Resize the circle by dragging the dot on the edge of the circle (or you can change the radius value in the fence panel).

- Polygon region:

  ![Polygon Geofence](../../../assets/plan/geofence/geofence_polygon.jpg)

  - Move the vertices by dragging the filled dots
  - Create new vertices by clicking the "unfilled" dots on the lines between the filled vertices.

1. By default new regions are created as _inclusion_ zones (vehicles must stay within the region).
   Change them to exclusion zones (where the vehicle can't travel) by selecting the fence and unchecking **Inclusion fence** in the fence panel.

## Edit/Delete a GeoFence

The **Fences** section of the GeoFence panel lists the polygon and circular fences, with their number of points and area or radius.
Exclusion fences have a filled icon. Fences added from geozones are named after their zones and listed separately.

Select a fence to edit by clicking it in the list, which also moves the map to it, or by clicking it on the map. Clicking where fences overlap selects them in turn.
You can then edit the region on the map as described in the previous section, change whether it is an inclusion fence and, for circles, set the radius.

Fences are deleted with their trash icon, **Delete Geozone Fences** deletes the fences added from geozones and **Delete All Fences** deletes all of them.

The panel also shows the number of fence points (polygon vertices, one point per circle and the breach return point). For PX4 vehicles, which store at most 64 points, it shows how much of the storage the fence uses. For ArduPilot vehicles, it warns about polygons with more than 255 vertices.

## Upload a GeoFence

The GeoFence is uploaded in the same way as a mission, using **File** in the [Plan tools](../plan_view/plan_view.md).

## Geozones (ED-269 / ED-318)

_QGroundControl_ can load UAS geographical zone (geozone) files published by aviation authorities in the EUROCAE ED-269 (JSON) or ED-318 (GeoJSON) format, AIXM 5.1 airspaces, the temporary flight restrictions (TFRs) published by the FAA, and the zones of the German DiPUL and French IGN map services.
The zones are shown on the map in both the Plan and Fly views, colored by restriction: prohibited (red), authorisation required (orange), conditional (yellow), no restriction (green) and U-space airspace (blue).
Zones whose published restriction isn't recognised are shown as unknown, and are checked and alerted like conditional zones.
Zones that are not active at the current time are drawn with a fainter fill. Schedules based on sunrise, sunset or civil twilight are computed for the location of each zone.

When zones are loaded, the **Geozones** button in the Plan View tool strip and the geozone indicator in the Fly View toolbar show a color legend, the number of loaded zones and sources, and any outdated data warning. The Plan View panel also has a **Show geozones** switch, and the expanded Fly View indicator has the display and flight alert settings. Both have **Manage Geozones...** to open the geozone settings.

To add geozones:

1. Open _Application Settings > Geozones_, or press **Manage Geozones...** in the Plan View **Geozones** panel or the Fly View geozone indicator.
1. Press **Add File...** and select a `.json` or `.geojson` file, or press **Add Feed...**, select a national feed (or **Other URL** to enter an `https://` address) and press **Download**.

Several files and feeds can be loaded at the same time. Files and downloads larger than 100 MB are rejected. Zip archives are read directly; when a package holds several geozone files (`.json`, `.geojson` or AIXM `.xml`), they are loaded together as one source, zones published in more than one file are kept once, and files of another format, like metadata, are left out. When a server answers with an error document or a web page instead of zones, its message is shown. Loading the same file again (from the same folder), or a feed with the same URL, replaces its zones; files with the same name in different folders are separate sources.
Files are loaded in the background, and the Plan View map is moved to the new zones if they are not in view.
_QGroundControl_ keeps a copy of each source and reloads them automatically on startup. Feeds are checked for updates when they are older than 24 hours (15 minutes for TFRs) by default, or sooner when the server tells how long its data stays current (HTTP `Cache-Control: max-age` or `Expires`) and that is shorter, though not more often than every 5 minutes. **Refresh Feeds** checks them immediately. Feeds which have not changed on the server are not downloaded again and keep the date of their data, and a feed republished with only new dates or a new title keeps its zones. If a download fails, the previous data is kept and the error, such as an untrusted certificate or a server which didn't respond within 2 minutes, is shown below the source. A failed feed is tried again after 5 minutes, then twice as long after each further failure, up to 6 hours or its refresh interval; **Refresh Feeds** tries at once.
If a new copy can't be saved, for example because the disk is full, the previous copy is kept and a message is shown. If the list of stored sources can't be read, the stored copies are moved to a `backup-<date>` folder in the `GeoZones` folder of the application data location so they can be added again; a list saved by a newer version of _QGroundControl_ is left untouched.

The national feeds also include **Germany aviation zones (DiPUL)**, the control zones, airports, aerodromes and flight restriction areas of the German DiPUL service (licensed CC BY-ND 4.0), and **United Kingdom FRZ (NATS)**, the flight restriction and runway protection zones of protected aerodromes and the danger areas from the AIXM dataset NATS publishes every AIRAC cycle. German zone types are mapped to restrictions: restricted areas and temporary no-fly zones are prohibited, aviation, sensitive and nature zones require authorisation, and residential areas, roads, railways and waterways are conditional.

Some services serve their zones by area. In the Plan View **Geozones** panel, **Download zones for the map area** downloads the zones of the map area, up to about 100 km wide, from **Germany, all zones (DiPUL)** (including residential areas, hospitals and nature reserves) or **France, open category (IGN)** (where flights are prohibited or limited to a maximum height). The provider covering the map is chosen. Downloaded areas are kept for offline use and refreshed like feeds. When the server limits its responses, the further pages are downloaded too (up to 50), and a warning is shown below the source if zones are still missing.
European NOTAMs can't be loaded, since EUROCONTROL only provides them to organisations which signed its data user agreement.

The **United States TFRs (FAA)** feed lists the current temporary flight restrictions. The details of each NOTAM are downloaded with the list for its effective times and altitudes, and kept for offline use; only new or changed NOTAMs are downloaded again. A TFR whose details can't be downloaded is treated as prohibited at all times and altitudes. Where a NOTAM has several areas, its TFRs span the altitudes of all of them. Check the NOTAM for exceptions.

Each source shows its format, zone count and date, for feeds when the server last confirmed the data current (**checked**), and after an update which changed zones, the zones added, removed and changed. A vehicle near or in a zone whose data changed is alerted again, since its limits or schedule may differ. A warning is shown if the data has expired (ED-318 `validTo`), is not valid yet, or is more than 28 days old by default (2 hours for TFRs, based on the ED-318 or FAA issue time, or else the file date or when the server last confirmed the downloaded data). Outdated sources are also reported once when _QGroundControl_ starts and in the Plan View warning, since they can miss zones.

Use **Remove** to remove a source and **Remove All** to remove all of them. **Show geozones** in the Plan View **Geozones** panel (or **Show geozones on maps** in the expanded Fly View indicator and the settings) hides or shows the zones.
The check box of a source turns it off or on, in the settings and, with several sources, in the Plan View **Geozones** panel and the Fly View indicator. The zones of a source turned off are not shown, checked or alerted, its outdated data isn't reported, and it stays off after a restart until it is turned on or added again.
The check boxes of the legend hide or show the zones of each restriction on the maps, for example to see the map under large U-space airspace. Hidden zones are not picked by map clicks, but the mission check, flight alerts and guided command checks still apply to them.
Click a zone on the map to see its details (restriction, altitude limits in your units, applicability schedule, reason, message, responsible authority, exemptions, U-space class, other published properties and the source it was loaded from).
Problems found while reading a source are shown below it and in the details of the zones concerned: altitude limits in an unknown unit are ignored, so the zone applies at all altitudes, and unknown days in a schedule are taken as every day. Zone shapes are checked too: repeated points are dropped and boundaries need not be closed, but polygons with fewer than 3 distinct points or without area, polygons touching or enclosing a pole (within about 11 km of it) and circles without a positive radius are ignored, with a warning naming them. A zone left without any shape is counted as skipped. An invalid hole is ignored, so the zone covers it. A polygon whose boundary crosses itself is kept with a warning; the parts it encloses twice (like the center of a star drawn in one stroke) are outside the zone. Where zones overlap, choose the zone at the top of the details. In the Plan View this works when the click doesn't add a waypoint or rally point or select a fence. In the Fly View, when guided actions are available, the map click menu has **Geozones here**. When the map is zoomed in, each zone also shows a label which opens its details.
To find a zone, type part of its name or identifier in the search field of the Plan View **Geozones** panel or the Fly View geozone indicator, then pick a result to move the map to the zone and show its details.
When many small zones are in view, the smallest are left out until you zoom in, and the Plan View **Geozones** panel shows how many are hidden.

### Mission and Flight Checks

When the planned mission path or the area of a survey, corridor scan or structure scan enters a prohibited, authorisation required or conditional zone, a warning listing the zones is shown at the top of the Plan View map and the zones are outlined in white. Click the warning to show the zones on the map.
Besides the drawn flight path, the check follows loiter circles with a radius, the loiter of landing patterns, the distance VTOL transitions fly on past their waypoint (**VTOL TransitionDistance** in _Application Settings > Plan View_), the loops of **DO_JUMP** items, and the return the vehicle flies after a **Return To Launch** item (see below). The warning names what enters zones the drawn flight path doesn't, like "Only the loiter at item 3 enters ..." or "Only the jump at item 8 enters ...".
A **DO_JUMP** flies the items from its target to the jump again as many times as its repeat count, including the straight leg back to the target, so zone schedules are checked at the times the repeats are flown. A jump repeating without end is flown once more and named "repeated without end"; very long repeats are cut off after about 5000 legs.
U-space airspaces the mission enters are listed too, since flying there requires a flight authorisation from a U-space service provider. Zones without restriction are listed for their conditions, they never hide restricted zones.
Uploading a mission which enters a prohibited zone asks for confirmation.

If you hold a permit for a zone, press **Add Authorisation...** in its details and enter the permit number and, optionally, its first and last days. While the authorisation is valid, the zone is listed as authorised instead of warned about by the mission check, and the flight alerts, guided command checks and **Route Around Zones...** leave it out. With **Check zones at the flight time**, the authorisation must cover the whole flight. Authorisations apply to the zone in every source, are listed in the **Authorisations** section of _Application Settings > Geozones_ where they can be removed, and are kept after a restart.
Saved plans keep the flight time, the authorisations of the zones the mission enters and the version of the geozone data (see [Plan Data Audit](#plan-data-audit)). When a plan with authorisations you don't have is opened, the Plan View banner lists them with **Add** and **Ignore**, so permits from someone else's plan are only added when you choose to.
If the vehicle supports a GeoFence, the warning also names the prohibited zones on the path which no exclusion fence covers, and **Add Exclusion Fences** adds them to the GeoFence so the vehicle stops at them. Fences added from a zone cover it whatever their simplification, while a fence which leaves out part of a zone, or a zone without area, doesn't count.
**Route Around Zones...** in the warning suggests, for each leg between waypoints which enters restricted zones, a detour keeping 100 m from them, which **Insert** adds as waypoints, and the zones which don't reach the ground and can be flown below. Detours go around each zone separately, so they can pass between zones. Detours are checked at the altitudes of the leg, over the terrain along the detour where its data is loaded. For a zone which can be flown below, **Apply** lowers the waypoints of the leg to 10 m below its floor when they use relative or AMSL altitudes and the terrain along the leg is known, as long as the leg stays at least 10 m above the terrain. Check the new waypoints, altitudes, the terrain and obstacles before flying.
The altitude profile below the Plan View map shades the restricted zones and U-space airspaces along the path between their floors and ceilings, so you can see where to climb or descend. Limits above ground follow the terrain, and limits that are unknown or beyond the chart extend to its edges.
The check compares the planned altitudes with the zone altitude limits, using the terrain elevation along the path for limits above ground. Between waypoints the altitude is taken to change evenly, so a leg which descends below a zone's ceiling only after passing it doesn't enter the zone. When the terrain or altitude is not known, the zone is assumed to apply. By default zone schedules are ignored, since the mission may be flown at another time. To check a flight at a known time, turn on **Check zones at the flight time** in the Plan View **Geozones** panel and enter the local start time (**Now** sets the current time). The mission check, the altitude profile and **Route Around Zones...** then leave out zones which aren't active while the mission flies through them, estimating when it does from its duration as if the mission is flown at an even pace. Since the take-off may be early or late and the pace may differ, a zone still counts when it is active within 15 minutes before or after the estimated times it is flown through. The warning shows the time and how many zones were left out.

**Return and contingency paths.** The return paths follow the parameters of the connected vehicle, read again whenever they, the rally points, the breach return point or the planned home position change:

- **PX4**: `RTL_TYPE` chooses the destination: 0 the closest of home and the rally points, 1 the closest rally point or mission landing (home without them), 3 the closest of home, the rally points and the mission landing, 5 the closest rally point (landing in place without one), 6 like 0 since the battery state is unknown. Types 2 and 4 continue the mission to its landing (no other path) or fly it in reverse and then home from the first waypoint. A destination must be 2 m closer than the next best one, like PX4 requires. The vehicle climbs to `RTL_RETURN_ALT` above the destination (or returns at its altitude when higher), descends to `RTL_DESCEND_ALT` at the destination and lands, or loiters there when `RTL_LAND_DELAY` is negative; fixed wings circle at `NAV_LOITER_RAD`. Multicopters lower the return altitude within `RTL_MIN_DIST` of the destination to the cone of `RTL_CONE_ANG`, at least twice `NAV_ACC_RAD` above it. A mission landing is a `DO_LAND_START` followed by a landing: the vehicle flies to its first waypoint, then the landing is part of the mission.
- **ArduCopter**: home or the closest rally point (`RALLY_INCL_HOME`, `RALLY_LIMIT_KM`). The vehicle climbs to `RTL_ALT` above home, at least `RTL_CLIMB_MIN` above its altitude and never lower than it (`RTL_ALT` 0 returns at the current altitude), limited by `RTL_CONE_SLOPE` near the destination and by the altitude fence (`FENCE_ALT_MAX` less `FENCE_MARGIN` when enabled), then descends to `RTL_ALT_FINAL` above home or lands. Both the centimeter parameters until Copter 4.6 and the meter parameters `RTL_ALT_M`, `RTL_CLIMB_MIN_M` and `RTL_ALT_FINAL_M` of later versions are read.
- **ArduPlane**: home at `RTL_ALTITUDE` above home (`ALT_HOLD_RTL` in centimeters on older versions, -1 for the current altitude) or the closest rally point at its altitude, climbing at once when lower and descending along the way when higher, then circling at `RTL_RADIUS` (or `WP_LOITER_RAD`). With `RTL_AUTOLAND` 1 the plane then flies the landing sequence of the closest `DO_LAND_START`, with 2 it flies to it directly, with 4 it joins the closest waypoint after a `DO_RETURN_PATH_START`.
- Other vehicles return straight to the closest of home and the rally points at their return altitude parameter.

Until the parameters of a connected vehicle are loaded, and without a connected vehicle, the check assumes the defaults of the firmware and vehicle class of the plan (**Vehicle Info** in the Mission Start item without a connected vehicle): `RTL_RETURN_ALT` 30 m and `RTL_DESCEND_ALT` 10 m for PX4 multicopters, `RTL_TYPE` 1, `RTL_RETURN_ALT` 100 m and loitering for PX4 fixed wings, `RTL_TYPE` 1 and `RTL_RETURN_ALT` 60 m for PX4 VTOLs (the airframe defaults, which differ from the parameter defaults of 0, 60 m and 30 m), `RTL_ALT` 15 m with `RTL_CONE_SLOPE` 3 for ArduCopter and `RTL_ALTITUDE` 100 m for ArduPlane, without rally limits or fences beyond their defaults. The warnings say so, like "assuming the PX4 default return altitude of 30 m". For other firmware the return altitude is unknown, zones along the way count at any altitude and the warnings say "at an unknown altitude".

The check also follows the contingency paths: the return the vehicle would fly from each item of the mission and from points every 250 m or more between them (at most 100 starts), after a failsafe or a fence breach. With ArduPlane's `FENCE_ACTION` 6 or 7 (guided) it also follows the flight to the breach return point of the GeoFence (home without one, the closest rally point with `FENCE_RET_RALLY`) at `FENCE_RET_ALT`, between the fence altitudes, or at the return altitude. Fence actions which return to launch (PX4 `GF_ACTION` 3, ArduPilot `FENCE_ACTION` 1) are covered by the returns, other actions hold, brake, land or retrace the path flown (SmartRTL) and add no path. Restricted zones only contingency paths enter are listed apart and grouped by the items they would be flown from, like "A return to launch from items 3–7 would enter ...". Unlike the planned flight they don't ask for confirmation on upload.

The return paths are estimates: wind, the turns and climb rate of fixed wings, terrain following returns (`RTL_ALT_TYPE`), PX4's geofence avoidance, SmartRTL, QuadPlane returns (`Q_RTL_MODE`), VTOL approach loiters and ArduPilot's checks of rally points against the fence aren't modeled. PX4 VTOLs are taken to return in fixed wing mode, without the cone, landing at the destination.

While flying, _QGroundControl_ warns once (with a message and audio) when a connected vehicle comes within 100 m (or 30 m vertically, by default) of an active prohibited, authorisation required, conditional, unknown or U-space zone, or would enter it within 30 seconds at its current speed, direction and climb rate, and again when it enters the zone. Alerts based on the velocity give the time to entry, so fast vehicles are warned in time. The velocity is the one the vehicle sends with its position, or is estimated from its positions when it sends none. While a vehicle's link or position is lost, its alerts pause, since its last position isn't where it is, and the geozone indicator says so. Limits above ground use the vehicle's height above terrain, and the predicted path and guided commands use the terrain ahead where its data is loaded, so rising ground towards a zone counts. Without terrain data for the vehicle position, zones with limits above ground are assumed to apply at any altitude once the vehicle is armed; a disarmed vehicle is taken to be on the ground.
Guided commands of the Fly View are checked too: the confirmation of **Go to location**, **Orbit**, **Change altitude**, **Takeoff** and starting or continuing the mission names the active restricted zones and U-space airspaces the vehicle would enter, including zones it is in which it would leave and enter again, updated as the altitude slider moves, and a go to location which enters zones is always confirmed. Starting the mission also checks the mission on the vehicle for a flight starting now and whether the vehicle's GeoFence excludes its prohibited zones.
While zones are above or below the active vehicle, a strip on the left of the Fly View shows their floors and ceilings with the vehicle's altitude, above the ground when the terrain is known.
Alerts are shown in a popup below the toolbar, the most restrictive zone first, and spoken. Alerts raised together, also by several vehicles, are spoken as one message naming the two most restrictive and counting the others ("..., and 2 more"). Spoken messages are at least 5 seconds apart, unless a more urgent alert comes: a more restrictive zone, or entering rather than approaching it. While a vehicle stays inside a prohibited zone, "Still inside geozone ..." is spoken and shown again every 30 seconds until the alert is acknowledged or the vehicle leaves the zone. **Acknowledge** in the popup acknowledges an alert, as do **Show** and **Acknowledge All** in the indicator. **Mute Alerts for 1 min** in the popup silences the spoken alerts for a minute (the indicator shows it, with **Unmute**); entering a prohibited zone is still announced once, and alerts raised while muted are not spoken later.
When a vehicle reports a breach of its own fence (MAVLink `FENCE_STATUS`), _QGroundControl_ shows it in the alert popup, the geozone indicator turns red and its page names the kind of breach (boundary, minimum or maximum altitude) and how many breaches the vehicle counted, until the vehicle reports the breach cleared.
When the active vehicle would enter a zone within the approach alert time at its current velocity, the Fly View map draws its path to the entry point in the zone's color, with the zone name and the seconds left.
Every connected vehicle is monitored on its own, and alerts name the vehicle when several are connected. A vehicle which reconnects starts afresh, even with the same system id, so its alerts repeat. The geozone indicator in the Fly View toolbar turns red, orange, yellow or blue when the active vehicle is near or inside a zone, and is dimmed while no vehicle is monitored. Click it to see the nearby zones; expand it to change the display and flight alert settings.

Alerts are also appended to a `GeoZoneAlerts-<date>.csv` file per day (UTC) in the _Telemetry_ folder of the application load/save path, with the time, vehicle, position, altitude, terrain elevation, event, distance, zone and predicted time to entry, for reviewing flights.
The indicator lists the **Recent Alerts** of the session, newest first, followed by those logged earlier the same day. Alerts not looked at yet are shown in bold and counted in a red badge on the indicator; **Acknowledge All** clears them. **Show** opens the details of the alert's zone and marks the vehicle position at the time of the alert on the map for two minutes (click the mark to remove it).

Altitude limits relative to the WGS84 ellipsoid are converted using the EGM96 geoid, within a few meters.

How a zone boundary runs between two published points depends on the format. For long edges, especially at high latitudes, a great circle (geodesic) and a line straight in latitude and longitude can be several hundred meters apart:

- **ED-269, ED-318, DiPUL and IGN GeoJSON**: straight in latitude and longitude, as GeoJSON defines its lines (RFC 7946, section 3.1.1). ED-269 polygons are GeoJSON polygons and ED-318 uses the GeoJSON geometries; neither standard defines its edges otherwise.
- **AIXM 5.1**: `GeodesicString` segments are great circles. `LineStringSegment` segments in EPSG:4326 are straight in latitude and longitude, which the AIXM coding guidelines use for boundaries along parallels. Arcs (`ArcByCenterPoint`) are circles of the published radius around their center, with their angles taken as bearings.
- **FAA TFRs**: the NOTAM details code the boundary points of each TFR as great circle points (`GRC`), so its edges are great circles when the details are available, and straight otherwise. Circular TFRs are published as 36-sided polygons, whose sides cut slightly inside the circle (by about 7 m for a radius of 1 NM); when the NOTAM details give the circle (its center and radius), the zone is that circle instead. Polygons approximating circles in other sources are used as published.

Edges are checked as straight lines in latitude and longitude, so when zones are loaded, great circle edges get extra points on their great circle (up to 1023 per edge) and arcs a point every few degrees, keeping the boundary used by the checks, maps and fences within 1 m of the published one. On maps, long straight edges get extra points too, since the map projection would otherwise move them.

Limits published as flight levels (pressure altitude, `STD`) are approximate: their height depends on the air pressure and temperature. They are converted to heights above mean sea level with the **QNH** setting, as an altimeter set to it would read them in the ICAO standard atmosphere: the pressure altitude less 44330.8 m × (1 − (QNH / 1013.25 hPa)^0.190263), about 8.3 m lower per hPa below 1013.25 hPa. The zone details show both, like "FL 95 ≈ 2810 m AMSL (QNH 1003 hPa)". Since flight levels are lower in air colder than standard (by about 4 % of their height per 10 °C), the checks, alerts, altitude profile and strip lower floors and raise ceilings given as flight levels by 4 % of their height. Set the local QNH before planning or flying near such zones; a change applies at once to the checks and alerts.

### Pre-flight Briefing

**Export Briefing...** in the Plan View **Geozones** panel or in the geozone banner above the map saves a briefing of the geozones of the planned mission. The file dialog proposes the folder and name of the open plan with `-briefing.pdf`; the extension of the file name chooses the format:

- **PDF** (`.pdf`): an A4 document to print or take along, with the mission name, the date and page numbers on each page.
- **Web page** (`.html`): a single file without external resources, which reads well on a phone and prints cleanly.
- **JSON** (`.json`): the same content for other tools, see below.

The briefing lists the mission (vehicle and firmware, planned start and estimated end, duration, distance, highest altitude, home and how the returns were modeled), then:

- **Zones the flight path enters**, in the order the mission enters them: where (after which item, how far along the mission, the estimated times with a planned start), the altitudes flown inside against the zone floor and ceiling at the entry, when the zone is active during the flight (widened by 15 minutes like the mission check) and its published schedule, the vertical limits as published, the authority and its contacts, the message and conditions, U-space class, exemptions, your authorisation with whether it covers the flight, and the source and content hash of the zone.
- **Zones only returns would enter**, with the returns and the items they would be flown from.
- **Restricted zones within 500 m** of the flight path, or passed above or below it, with their distance.
- **Data versions**: each source covering the mission with its format, file or feed, data (issue) date, the AIRAC cycle in effect at that date (not for live restrictions), its published validity, when it was last checked, and the hash of its content.

The JSON file has `"schema": "qgc-geozone-briefing"` and `"version": 1`. The version increases when fields are removed or change meaning, not when fields are added. Times are ISO 8601 UTC, distances and altitudes meters (altitudes above mean sea level), unknown values `null`. The fields are:

- `generated`, `generator`, `scheduleToleranceSeconds`, `nearbyDistanceMeters`.
- `mission`: `name`, `planFile`, `vehicle`, `firmware`, `plannedStart`, `plannedEnd`, `durationSeconds`, `distanceMeters`, `maxAltitudeAMSL`, `maxAltitudeAboveHome`, `home` (`lat`, `lon`, `altitudeAMSL`), `itemCount`, `returnAssumptions` (texts).
- `zones`: `involvement` (`entered`, `contingency` or `nearby`), `key`, `identifier`, `country`, `name`, `restriction` (`prohibited`, `req_authorisation`, `conditional`, `no_restriction`, `u_space` or `unknown`), `restrictionCode` (as published), `restrictionText`, `reasons`, `verticalLimits`, `schedule`, `activeDuringFlight`, `activePeriods` (`start`, `end`), `crossings` (`path`: empty for the flight path or what the vehicle does, like "the loiter at item 3", `entryItem` and `exitItem`: sequence number of the last item reached before, `entryDistance`, `exitDistance`, `entry` and `exit` (`lat`, `lon`), `entryTime`, `exitTime`, `pathAltitudeMin`, `pathAltitudeMax`, `zoneFloor`, `zoneCeiling`, `active`), `inArea`, `contingencies` (texts), `distance`, `passed` (`above`, `below` or `null`), `authorities` (texts), `message`, `conditions`, `otherReasonInfo`, `uSpaceClass`, `regulationExemption`, `authorisation` (`permit`, `validFrom`, `validTo`, `status`, `coversFlight`, or `null`), `source` and `contentHash` (hexadecimal).
- `sources`: `key`, `name`, `format`, `provider`, `dataDate`, `loaded`, `checked`, `validFrom`, `validTo`, `airacCycle`, `contentHash` (hexadecimal).

### Plan Data Audit

When a plan is saved while geozone data is loaded, the plan file keeps, in the `audit` object of its `geoZones` section, the version of each source covering the mission (data date and content hash) and the content hash of each zone the mission or a return from along it enters, whatever its schedule.
When the plan is opened again, and whenever the sources reload while it is open, the zones are compared with the current data. If they differ, the banner above the map warns, like "Geozone data changed since this plan was saved: 2 zone(s) modified, 1 new. Review the plan.":

- **modified**: a zone of the plan whose published content (geometry, limits, schedule, restriction, name...) changed. Zones are matched by their source, identifier, name and restriction, or by their country and identifier when they were renamed, reclassified or moved to another source.
- **removed**: a zone of the plan no source has anymore, or whose source was removed or disabled.
- **new**: a zone the mission now enters which came with updated or newly loaded data. Zones the mission enters only because you edited it don't count.
- **source updated** or **removed**: the data of a source covering the mission changed, even when the zones of the mission didn't.

**Details** lists the changes and **Acknowledge** hides the warning until the data changes again. Saving the plan records the current data, so the warning disappears. Plans saved without this record, like older plans, open without warning.

### Geozone Settings

Besides the sources, the **Geozones** page of the _Application Settings_ sets:

- **Show geozones on maps**.
- **Flight alerts**: turns the flight alerts and the nearby zones of the toolbar indicator on or off, **Speak flight alerts** turns their audio on or off and **Log flight alerts** the CSV log.
- **Approach alert distance** and **Approach alert height margin**: how close horizontally and vertically the vehicle can come to a zone before it is alerted.
- **Approach alert time**: how many seconds before entering a zone, at its current velocity, the vehicle is alerted. 0 turns these alerts off.
- **QNH**: the local sea level pressure (altimeter setting, 850 to 1100 hPa) converting flight levels to heights above mean sea level. The default, 1013.25 hPa, is the standard pressure of flight levels.
- **Feed refresh interval** and **Live restrictions refresh interval** (TFRs): how often feeds are downloaded again.
- **Outdated data warning** and **Outdated live restrictions warning** (TFRs): the data age at which a source is reported as outdated.

::: warning
These checks are advisory. Always check the official sources and obtain any required authorisation before flying.
:::

### Add Geozones to the GeoFence

If the vehicle supports GeoFences, loaded zones can be converted into exclusion fences which fit into the vehicle's fence storage. Press **From Geozones...** below **Polygon Fence** and **Circular Fence** in the GeoFence panel, then in the dialog:

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

::: warning
GeoFences have no altitude limits, so a zone's vertical limits are not transferred and the whole zone is excluded at all altitudes.
Holes in zone polygons are also dropped. The ArduPilot fence storage is estimated, so verify the upload.
:::

## Remaining tools

The rest of the tools work exactly as they do while editing a Mission.
