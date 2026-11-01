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

## Add Geozones to the GeoFence

If geozones are loaded (see [Geozones](../geozones/geozones.md)), **From Geozones...** below **Polygon Fence** and **Circular Fence** converts the zones you select into exclusion fences which fit into the vehicle's fence storage, and can upload the fence and check that the vehicle stores it as planned.
The dialog, the fence storage of each autopilot and what isn't transferred, like the altitude limits of the zones, are described in [Add Geozones to the GeoFence](../geozones/geozones.md#add-geozones-to-the-geofence).

## Remaining tools

The rest of the tools work exactly as they do while editing a Mission.
