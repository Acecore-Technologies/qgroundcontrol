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
   Change them to exclusion zones (where the vehicle can't travel) by unchecking the associated _Inclusion_ checkbox in the fence panel.

## Edit/Delete a GeoFence

You can select a geofence region to edit by selecting its _Edit_ radio button in the GeoFence panel.
You can then edit the region on the map as described in the previous section.

Regions can be deleted by pressing the associated **Del** button.

## Upload a GeoFence

The GeoFence is uploaded in the same way as a mission, using **File** in the [Plan tools](../plan_view/plan_view.md).

## Geozones (ED-269 / ED-318)

_QGroundControl_ can load UAS geographical zone (geozone) files published by aviation authorities in the EUROCAE ED-269 (JSON) or ED-318 (GeoJSON) format, and the temporary flight restrictions (TFRs) published by the FAA.
The zones are shown on the map in both the Plan and Fly views, colored by restriction: prohibited (red), authorisation required (orange), conditional (yellow), no restriction (green) and U-space airspace (blue).
Zones that are not active at the current time are drawn with a fainter fill. Schedules based on sunrise, sunset or civil twilight are computed for the location of each zone.

To add geozones:

1. Navigate to the Plan View and select the _Geofence_ radio button.
1. Expand the **Geozones (ED-269 / ED-318)** section of the GeoFence panel.
1. Press **Add File...** and select a `.json` or `.geojson` file, or press **Add Feed...**, select a national feed (or **Other URL** to enter an `https://` address) and press **Download**.

Several files and feeds can be loaded at the same time. Loading a file with the same name, or a feed with the same URL, replaces its zones.
Files are loaded in the background and the map is moved to the new zones if they are not in view.
_QGroundControl_ keeps a copy of each source and reloads them automatically on startup. Feeds are checked for updates when they are older than 24 hours (15 minutes for TFRs) by default, and **Refresh Feeds** checks them immediately. Feeds which have not changed on the server are not downloaded again. If a download fails, the previous data is kept and the error is shown below the source.

The **United States TFRs (FAA)** feed lists the current temporary flight restrictions. It doesn't include their times or altitudes, so every listed TFR is treated as prohibited at all altitudes; its details show the NOTAM to check for times, altitudes and exceptions.

Each source shows its format, zone count and date. A warning is shown if the data has expired (ED-318 `validTo`), is not valid yet, or is more than 28 days old by default (2 hours for TFRs, based on the ED-318 or FAA issue time, or the file or download date). Outdated sources are also reported once when _QGroundControl_ starts and in the Plan View warning, since they can miss zones.

Use **Show geozones on map** to hide or show the zones, **Remove** to remove a source and **Remove All** to remove all of them.
When the map is zoomed in, each zone shows a label; click it to see the zone details (restriction, altitude limits in your units, applicability schedule, reason, message, responsible authority and the source it was loaded from).

### Mission and Flight Checks

When the planned mission path or the area of a survey, corridor scan or structure scan enters a prohibited, authorisation required or conditional zone, a warning listing the zones is shown at the top of the Plan View map and the zones are outlined in white. Click the warning to show the zones on the map.
U-space airspaces the mission enters are listed too, since flying there requires a flight authorisation from a U-space service provider. Zones without restriction are listed for their conditions, they never hide restricted zones.
Uploading a mission which enters a prohibited zone asks for confirmation.
The check compares the planned altitudes with the zone altitude limits, using the terrain elevation along the path for limits above ground. When the terrain or altitude is not known, the zone is assumed to apply. Zone schedules are ignored, since the mission may be flown at another time.

While flying, _QGroundControl_ warns once (with a message and audio) when the active vehicle comes within 100 m (or 30 m vertically, by default) of an active prohibited, authorisation required, conditional or U-space zone, and again when it enters the zone. Limits above ground use the vehicle's height above terrain. Without terrain data for the vehicle position, zones with limits above ground are assumed to apply at any altitude.
The geozone indicator in the Fly View toolbar turns red, orange or yellow when the vehicle is near or inside a zone. Click it to see the nearby zones.

Altitude limits relative to the WGS84 ellipsoid are converted using the EGM96 geoid, within a few meters.

### Geozone Settings

The **Geozones** page of the _Application Settings_ sets:

- **Show geozones on maps**.
- **Flight alerts**: turns the flight alerts and the toolbar indicator on or off, and **Speak flight alerts** turns their audio on or off.
- **Approach alert distance** and **Approach alert height margin**: how close horizontally and vertically the vehicle can come to a zone before it is alerted.
- **Feed refresh interval** and **Live restrictions refresh interval** (TFRs): how often feeds are downloaded again.
- **Outdated data warning** and **Outdated live restrictions warning** (TFRs): the data age at which a source is reported as outdated.

::: warning
These checks are advisory. Always check the official sources and obtain any required authorisation before flying.
:::

### Add Geozones to the GeoFence

If the vehicle supports GeoFences, loaded zones can be converted into exclusion fences:

1. Check the zone types to add (**Prohibited**, **Authorisation required**, **Conditional**).
1. Leave **Only zones in current map view** checked to only add the zones visible on the map (recommended for national files that contain many zones).
1. Select when the zones must be **Active**: **Any time**, **Now**, or within the **Next 3 hours** or **Next 24 hours**.
1. Select a **Simplify** tolerance to reduce the number of fence points. Simplified fences always enclose the whole zone and extend up to twice the tolerance beyond it. Select **Off** to keep the published vertices.
1. Check the number of fences and points that will be added, then press **Add Exclusion Fences**. Fences that already exist are not added again.

If the resulting fence likely exceeds what the vehicle can store (64 fence points in total on PX4, 255 vertices per polygon on ArduPilot), you are asked to confirm before the fences are added.

::: warning
GeoFences have no altitude limits, so a zone's vertical limits are not transferred and the whole zone is excluded at all altitudes.
Holes in zone polygons are also dropped. ArduPilot fence storage depends on the flight controller and can be much smaller than the polygon limit, so only add the zones you need.
:::

## Remaining tools

The rest of the tools work exactly as they do while editing a Mission.
