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

Several files and feeds can be loaded at the same time. Files and downloads larger than 100 MB are rejected. Loading a file with the same name, or a feed with the same URL, replaces its zones.
Files are loaded in the background, and the Plan View map is moved to the new zones if they are not in view.
_QGroundControl_ keeps a copy of each source and reloads them automatically on startup. Feeds are checked for updates when they are older than 24 hours (15 minutes for TFRs) by default, and **Refresh Feeds** checks them immediately. Feeds which have not changed on the server are not downloaded again, and a feed republished with only new dates or a new title keeps its zones. If a download fails, the previous data is kept and the error, such as an untrusted certificate or a server which didn't respond within 2 minutes, is shown below the source. A failed feed is tried again after 5 minutes, then twice as long after each further failure, up to 6 hours or its refresh interval; **Refresh Feeds** tries at once.
If a new copy can't be saved, for example because the disk is full, the previous copy is kept and a message is shown. If the list of stored sources can't be read, the stored copies are moved to a `backup-<date>` folder in the `GeoZones` folder of the application data location so they can be added again; a list saved by a newer version of _QGroundControl_ is left untouched.

The national feeds also include **Germany aviation zones (DiPUL)**, the control zones, airports, aerodromes and flight restriction areas of the German DiPUL service (licensed CC BY-ND 4.0), and **United Kingdom FRZ (NATS)**, the flight restriction and runway protection zones of protected aerodromes and the danger areas from the AIXM dataset NATS publishes every AIRAC cycle. German zone types are mapped to restrictions: restricted areas and temporary no-fly zones are prohibited, aviation, sensitive and nature zones require authorisation, and residential areas, roads, railways and waterways are conditional.

Some services serve their zones by area. In the Plan View **Geozones** panel, **Download zones for the map area** downloads the zones of the map area, up to about 100 km wide, from **Germany, all zones (DiPUL)** (including residential areas, hospitals and nature reserves) or **France, open category (IGN)** (where flights are prohibited or limited to a maximum height). The provider covering the map is chosen. Downloaded areas are kept for offline use and refreshed like feeds, and a warning is shown below the source if the server left out zones beyond its limit.
European NOTAMs can't be loaded, since EUROCONTROL only provides them to organisations which signed its data user agreement.

The **United States TFRs (FAA)** feed lists the current temporary flight restrictions. The details of each NOTAM are downloaded with the list for its effective times and altitudes, and kept for offline use; only new or changed NOTAMs are downloaded again. A TFR whose details can't be downloaded is treated as prohibited at all times and altitudes. Where a NOTAM has several areas, its TFRs span the altitudes of all of them. Check the NOTAM for exceptions.

Each source shows its format, zone count and date, and after an update which changed zones, the zones added, removed and changed. A vehicle near or in a zone whose data changed is alerted again, since its limits or schedule may differ. A warning is shown if the data has expired (ED-318 `validTo`), is not valid yet, or is more than 28 days old by default (2 hours for TFRs, based on the ED-318 or FAA issue time, or the file or download date). Outdated sources are also reported once when _QGroundControl_ starts and in the Plan View warning, since they can miss zones.

Use **Remove** to remove a source and **Remove All** to remove all of them. **Show geozones** in the Plan View **Geozones** panel (or **Show geozones on maps** in the expanded Fly View indicator and the settings) hides or shows the zones.
The check box of a source turns it off or on, in the settings and, with several sources, in the Plan View **Geozones** panel and the Fly View indicator. The zones of a source turned off are not shown, checked or alerted, its outdated data isn't reported, and it stays off after a restart until it is turned on or added again.
The check boxes of the legend hide or show the zones of each restriction on the maps, for example to see the map under large U-space airspace. Hidden zones are not picked by map clicks, but the mission check, flight alerts and guided command checks still apply to them.
Click a zone on the map to see its details (restriction, altitude limits in your units, applicability schedule, reason, message, responsible authority, exemptions, U-space class, other published properties and the source it was loaded from).
Problems found while reading a source are shown below it and in the details of the zones concerned: altitude limits in an unknown unit are ignored, so the zone applies at all altitudes, and unknown days in a schedule are taken as every day. Where zones overlap, choose the zone at the top of the details. In the Plan View this works when the click doesn't add a waypoint or rally point or select a fence. In the Fly View, when guided actions are available, the map click menu has **Geozones here**. When the map is zoomed in, each zone also shows a label which opens its details.
To find a zone, type part of its name or identifier in the search field of the Plan View **Geozones** panel or the Fly View geozone indicator, then pick a result to move the map to the zone and show its details.
When many small zones are in view, the smallest are left out until you zoom in, and the Plan View **Geozones** panel shows how many are hidden.

### Mission and Flight Checks

When the planned mission path or the area of a survey, corridor scan or structure scan enters a prohibited, authorisation required or conditional zone, a warning listing the zones is shown at the top of the Plan View map and the zones are outlined in white. Click the warning to show the zones on the map.
Besides the drawn flight path, the check follows loiter circles with a radius, the loiter of landing patterns, the distance VTOL transitions fly on past their waypoint (**VTOL TransitionDistance** in _Application Settings > Plan View_), and a **Return To Launch** item flying straight home at the vehicle's return altitude parameter. Without a connected vehicle the return altitude is unknown, so zones along the way home count at any altitude. The warning names what enters zones the drawn flight path doesn't, like "Only the loiter at item 3 enters ...".
U-space airspaces the mission enters are listed too, since flying there requires a flight authorisation from a U-space service provider. Zones without restriction are listed for their conditions, they never hide restricted zones.
Uploading a mission which enters a prohibited zone asks for confirmation.

If you hold a permit for a zone, press **Add Authorisation...** in its details and enter the permit number and, optionally, its first and last days. While the authorisation is valid, the zone is listed as authorised instead of warned about by the mission check, and the flight alerts, guided command checks and **Route Around Zones...** leave it out. With **Check zones at the flight time**, the authorisation must cover the whole flight. Authorisations apply to the zone in every source, are listed in the **Authorisations** section of _Application Settings > Geozones_ where they can be removed, and are kept after a restart.
Saved plans keep the flight time and the authorisations of the zones the mission enters. When a plan with authorisations you don't have is opened, the Plan View banner lists them with **Add** and **Ignore**, so permits from someone else's plan are only added when you choose to.
If the vehicle supports a GeoFence, the warning also names the prohibited zones on the path which no exclusion fence covers, and **Add Exclusion Fences** adds them to the GeoFence so the vehicle stops at them.
**Route Around Zones...** in the warning suggests, for each leg between waypoints which enters restricted zones, a detour keeping 100 m from them, which **Insert** adds as waypoints, and the zones which don't reach the ground and can be flown below. Detours go around each zone separately, so they can pass between zones. Detours are checked at the altitudes of the leg, over the terrain along the detour where its data is loaded. For a zone which can be flown below, **Apply** lowers the waypoints of the leg to 10 m below its floor when they use relative or AMSL altitudes and the terrain along the leg is known, as long as the leg stays at least 10 m above the terrain. Check the new waypoints, altitudes, the terrain and obstacles before flying.
The altitude profile below the Plan View map shades the restricted zones and U-space airspaces along the path between their floors and ceilings, so you can see where to climb or descend. Limits above ground follow the terrain, and limits that are unknown or beyond the chart extend to its edges.
The check compares the planned altitudes with the zone altitude limits, using the terrain elevation along the path for limits above ground. Between waypoints the altitude is taken to change evenly, so a leg which descends below a zone's ceiling only after passing it doesn't enter the zone. When the terrain or altitude is not known, the zone is assumed to apply. By default zone schedules are ignored, since the mission may be flown at another time. To check a flight at a known time, turn on **Check zones at the flight time** in the Plan View **Geozones** panel and enter the local start time (**Now** sets the current time). The mission check and the altitude profile then leave out zones which aren't active at any time during the mission, using its estimated duration, and the warning shows the time and how many zones were left out.

While flying, _QGroundControl_ warns once (with a message and audio) when a connected vehicle comes within 100 m (or 30 m vertically, by default) of an active prohibited, authorisation required, conditional, unknown or U-space zone, or would enter it within 30 seconds at its current speed, direction and climb rate, and again when it enters the zone. Alerts based on the velocity give the time to entry, so fast vehicles are warned in time. The velocity is the one the vehicle sends with its position, or is estimated from its positions when it sends none. While a vehicle's link or position is lost, its alerts pause, since its last position isn't where it is, and the geozone indicator says so. Alerts raised together are listed in one message, the most restrictive zone first. Limits above ground use the vehicle's height above terrain, and the predicted path and guided commands use the terrain ahead where its data is loaded, so rising ground towards a zone counts. Without terrain data for the vehicle position, zones with limits above ground are assumed to apply at any altitude once the vehicle is armed; a disarmed vehicle is taken to be on the ground.
Guided commands of the Fly View are checked too: the confirmation of **Go to location**, **Orbit**, **Change altitude**, **Takeoff** and starting or continuing the mission names the active restricted zones and U-space airspaces the vehicle would enter, updated as the altitude slider moves, and a go to location which enters zones is always confirmed. Starting the mission also checks the mission on the vehicle for a flight starting now and whether the vehicle's GeoFence excludes its prohibited zones.
While zones are above or below the active vehicle, a strip on the left of the Fly View shows their floors and ceilings with the vehicle's altitude, above the ground when the terrain is known.
When the active vehicle would enter a zone within the approach alert time at its current velocity, the Fly View map draws its path to the entry point in the zone's color, with the zone name and the seconds left.
Every connected vehicle is monitored, and alerts name the vehicle when several are connected. The geozone indicator in the Fly View toolbar turns red, orange, yellow or blue when the active vehicle is near or inside a zone, and is dimmed while no vehicle is monitored. Click it to see the nearby zones; expand it to change the display and flight alert settings.

Alerts are also appended to a `GeoZoneAlerts-<date>.csv` file per day (UTC) in the _Telemetry_ folder of the application load/save path, with the time, vehicle, position, altitude, terrain elevation, event, distance, zone and predicted time to entry, for reviewing flights.
The indicator lists the **Recent Alerts** of the session, newest first, followed by those logged earlier the same day. Alerts not looked at yet are shown in bold and counted in a red badge on the indicator; **Acknowledge All** clears them. **Show** opens the details of the alert's zone and marks the vehicle position at the time of the alert on the map for two minutes (click the mark to remove it).

Altitude limits relative to the WGS84 ellipsoid are converted using the EGM96 geoid, within a few meters.

### Geozone Settings

Besides the sources, the **Geozones** page of the _Application Settings_ sets:

- **Show geozones on maps**.
- **Flight alerts**: turns the flight alerts and the nearby zones of the toolbar indicator on or off, **Speak flight alerts** turns their audio on or off and **Log flight alerts** the CSV log.
- **Approach alert distance** and **Approach alert height margin**: how close horizontally and vertically the vehicle can come to a zone before it is alerted.
- **Approach alert time**: how many seconds before entering a zone, at its current velocity, the vehicle is alerted. 0 turns these alerts off.
- **Feed refresh interval** and **Live restrictions refresh interval** (TFRs): how often feeds are downloaded again.
- **Outdated data warning** and **Outdated live restrictions warning** (TFRs): the data age at which a source is reported as outdated.

::: warning
These checks are advisory. Always check the official sources and obtain any required authorisation before flying.
:::

### Add Geozones to the GeoFence

If the vehicle supports GeoFences, loaded zones can be converted into exclusion fences. Press **From Geozones...** below **Polygon Fence** and **Circular Fence** in the GeoFence panel, then in the dialog:

1. Check the zone types to add (**Prohibited**, **Authorisation required**, **Conditional**).
1. Leave **Only zones in current map view** checked to only add the zones visible on the map (recommended for national files that contain many zones).
1. Select when the zones must be **Active**: **Any time**, **Now**, or within the **Next 3 hours** or **Next 24 hours**.
1. Select a **Simplify** tolerance to reduce the number of fence points. Simplified fences always enclose the whole zone and extend up to twice the tolerance beyond it. Select **Off** to keep the published vertices.
1. Check the number of fences and points that will be added, then press **Ok**. Fences that already exist are not added again.

If the resulting fence likely exceeds what the vehicle can store (64 fence points in total on PX4, 255 vertices per polygon on ArduPilot), you are asked to confirm before the fences are added.

::: warning
GeoFences have no altitude limits, so a zone's vertical limits are not transferred and the whole zone is excluded at all altitudes.
Holes in zone polygons are also dropped. ArduPilot fence storage depends on the flight controller and can be much smaller than the polygon limit, so only add the zones you need.
:::

## Remaining tools

The rest of the tools work exactly as they do while editing a Mission.
