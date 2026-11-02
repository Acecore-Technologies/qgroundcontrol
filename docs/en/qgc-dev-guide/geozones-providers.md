# Geozone Data Providers

This page records the research into airspace and no-fly zone data providers for the Acecore no-fly zone project (the _QGroundControl_ build for the Signav Pro controller), the data a provider must supply, the two ways users can get the data, and what is implemented for each.
It reflects the providers' public documentation as of November 2026.
How the sources are loaded is described in the [Geozones developer guide](geozones.md), what users see in the [Geozones user guide](../qgc-user-guide/geozones/geozones.md).

## Data Needed {#data-needed}

No-fly zones, restricted airspace and other flight limitations are loaded as UAS geographical zones (EUROCAE ED-269 / ED-318, see `GeoZone::Data`). A provider is usable when it supplies, per zone:

| Data | ED-269 / ED-318 field | Needed for | Required |
| ---- | --------------------- | ---------- | -------- |
| Identifier and name | `identifier`, `name` | Alerts, the alert log, briefings, telling zones apart across reloads | Yes |
| Restriction | `restriction`: `PROHIBITED`, `REQ_AUTHORISATION`, `CONDITIONAL`, `NO_RESTRICTION` (and U-space) | Map colors, mission check, flight alerts, fence import | Yes, or a type it can be derived from |
| Horizontal geometry | `geometry` / `horizontalProjection`: polygons, circles | Everything | Yes |
| Vertical limits | `lowerLimit`, `upperLimit` with their reference (`AGL`, `AMSL`, flight level) and unit (m, ft) | Height restrictions, like the ceiling of a control zone, the altitude strip, alerts at the flight altitude | Yes; without them a zone applies at all altitudes |
| Schedule | `applicability`: permanent, periods, daily times | Zones active only at times, mission start time | When zones aren't permanent |
| Reasons, message and conditions | `reason`, `message`, `restrictionConditions` | Telling the pilot why and how a zone applies | Recommended |
| Authority and contact | `zoneAuthority` (name, phone, email, site) | Requesting an authorisation | Recommended |
| Dataset metadata | `provider`, `issued`, `validFrom` / `validTo`, AIRAC cycle | Outdated data warnings, briefings, provenance | Recommended |
| Temporary restrictions | NOTAMs, TFRs | Short-lived no-fly zones (events, emergencies) | Yes, refreshed in minutes rather than days |

The data is cached for offline use and warned about when outdated: after 28 days by default, 2 hours for TFRs, or when its `validTo` or AIRAC cycle has passed.

## Provider Matrix {#provider-matrix}

| Provider | Coverage | Access | Licence / cost | Data model | ED-269 / ED-318 fit | In _QGroundControl_ |
| -------- | -------- | ------ | -------------- | ---------- | ------------------- | ------------------- |
| Official national feeds | Switzerland (FOCA), Sweden (LFV), Finland (Traficom), Latvia (LGS), Luxembourg, Germany (DiPUL WFS and the DFS ED-318 service), France (IGN WFS), United Kingdom (NATS AIXM) | Public downloads; the DFS ED-318 service needs a dipul account | Free; DiPUL CC BY-ND 4.0, others per publisher | ED-269 / ED-318 files and services, national GeoJSON, AIXM 5.1 | Native or close (restriction and limits per zone type) | Implemented: **Add Feed...**, **Download zones for the map area**, **Add Service...** |
| FAA (United States) | United States | Public, no account: ArcGIS feature services (UAS facility maps, national security UAS restrictions, class and special use airspace), `tfr.faa.gov` TFRs. NOTAM API (NMS): OAuth2 client credentials on request | US government data, public domain with a disclaimer (not for navigation); ArcGIS rate limit of 6000 request units a minute | GeoJSON of ArcGIS layers and of the TFR WFS, NOTAM details XML | Mapped per layer: facility map cells require authorisation up to their LAANC ceiling (0 ft prohibited), national security restrictions and prohibited areas are prohibited, controlled airspace and restricted areas require authorisation | Implemented (format `FaaAirspace`, `FaaTfr`); NMS NOTAMs configuration only |
| OpenAIP | Worldwide, community maintained | Free account and API key (`x-openaip-api-key` header or `apiKey` parameter), `GET /api/airspaces` with `bbox` (up to 5°), `page` and `limit`; daily per-country GeoJSON exports without a key | CC BY-NC 4.0: attribution, non-commercial use; rate limited | Airspace items: numeric `type` and `icaoClass`, `lowerLimit` / `upperLimit` (value, unit m/ft/FL, datum GND/MSL/STD), `hoursOfOperation`, GeoJSON polygon | Partial: conventional ICAO airspace, no UAS restriction or reasons. Mapped from the type (prohibited, restricted, CTR, danger areas...), class E–G and en-route airspace left out | Implemented as unofficial data (format `OpenAip`), never enabled by default: **Add Service...** (API key, by area) or the country export |
| Airspace Link (AirHub) | United States | OAuth2 client credentials and an `x-api-key` header; sandbox and production hosts; access through sales (free trial) | Commercial licence, content proprietary | `POST /v2/briefing/b4ufly/area` with a GeoJSON boundary answers notices: `dataSource`, `dataSubtype` (class B–E, prohibited, restricted...), `code`, `lowerAltitude` / `upperAltitude` (ft AGL), `schedule`, `contact`, optional geometry; vector tiles; LAANC operations | Good for authority, message, schedule and geometry; restriction and reasons derived from the notice codes; feet AGL only; a briefing rather than a zone feed | Configuration only (template **United States advisories (Airspace Link)**): needs a POST-geometry request and a notices parser, about 2–3 days once credentials are available |
| Unifly | EU and others: powers DFS dipul (Germany), skeyes DroneGuide (Belgium), NAV CANADA, ENAIRE | No public developer documentation; Operator and Platform APIs for partners | Enterprise agreements, not published | Not public | Can't be assessed | Not implemented. The official data it serves is available from the national feeds (DiPUL WFS, DFS ED-318) |
| Skylark | Unidentified. The only matching product is Skylark Drones' Watchtower (India): a free airspace map and paid APIs (tiles, advisories) with token authentication | No public API reference | Pay as you go | DGCA Digital Sky green / yellow / red zones | Unknown | Not implemented: confirm with Acecore which product the spec means |
| Altitude Angel | United Kingdom | GuardianUTM developer API (OAuth2) | — | — | — | Not integrated: the company entered administration on 7 October 2025, its developer site no longer resolves and its repositories are archived; Indra Group bought the GuardianUTM platform in January 2026 without a published continuation plan for API users. The spec also raises doubts about its data. |

## User Flows {#user-flows}

The spec asks whether Acecore aggregates the data and sends it to its users, or whether each user subscribes to providers from a list implemented in _QGroundControl_ ("option 1 / self sign-on").

### Acecore Aggregates and Distributes {#flow-aggregate}

Acecore collects the data from the providers on a backend, normalizes it to ED-318 and distributes it to its controllers as signed packs or as a feed.

- Pros: one data set Acecore has checked, fleet-wide consistency, offline use by design, no credentials on the controllers.
- Cons: Acecore needs redistribution rights from each provider (OpenAIP forbids commercial use, DiPUL forbids derivatives, Airspace Link's content is proprietary), runs and secures the backend, and carries the freshness of the data: temporary restrictions must reach the controllers within minutes.

### Self Sign-on (Option 1) {#flow-self-sign-on}

Each user adds providers from the list in **Application Settings > Geozones** and signs in with their own account or subscription; _QGroundControl_ downloads and caches the zones around the mission, the vehicle and the map view.

- Pros: the licence is between the user and the provider, the data comes straight from its publisher and is as fresh as its refresh interval, nothing to operate for Acecore.
- Cons: users manage accounts; providers without a public API (Unifly) can't be offered; secrets are stored in the settings, unencrypted.

### Recommendation {#recommendation}

Self sign-on, as the spec prefers, with the free official sources pre-populated so that most users need no account: the national ED-269 / ED-318 feeds in Europe and the FAA layers and TFRs in the United States. Commercial providers are added as service templates once Acecore has partner access (Airspace Link first, for LAANC advisories). OpenAIP fills gaps for non-commercial users only, marked unofficial. Signed packs from Acecore complement this for fleets flying offline or in regions without a public source, and are the way to distribute data Acecore curates.

### What Is Implemented {#implemented}

| Flow | Implemented | Missing |
| ---- | ----------- | ------- |
| Self sign-on | **Add Feed...** with the national feeds; **Add Service...** templates: ED-318 services (DFS with a dipul account), zones of an area by GET, FAA layers (no account), OpenAIP (API key), FAA NMS and Airspace Link (configuration only); API key header or query, bearer, basic and OAuth2 client credentials authentication, the secret kept per source; downloads for the mission, the vehicle and the map view (debounced, up to 200 km), caching, refresh scheduling, outdated data warnings; **Download zones for the map area** for the WFS and FAA providers | Airspace Link and NMS parsers and the POST-geometry request; encrypted secret storage (Android keystore on the Signav Pro); a sign-in UI per provider beyond the generic service form |
| Acecore aggregation | Offline packs with the stored copies and their provenance, **Export Pack...** / **Import Pack...**, Ed25519 signatures with trusted publishers; feeds and ED-318 services from an Acecore server, checked against a published SHA-256 | The aggregation backend; importing packs from a URL with periodic updates; push of temporary restrictions |

## Implementing a Provider {#implementing-a-provider}

See [Adding a Format](geozones.md#adding-a-format) and [Adding a Provider or Service](geozones.md#adding-a-provider). The FAA (`GeoZoneFaaAirspaceFormat`) and OpenAIP (`GeoZoneOpenAipFormat`) formats show how data without UAS restrictions is mapped: the restriction from the airspace type, the reasons from its purpose, informational or unofficial data said in the zone details (`extendedProperties`) and the dataset provider, which briefings list.
