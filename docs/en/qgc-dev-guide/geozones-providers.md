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
| Official national feeds | Switzerland (FOCA), Sweden (LFV), Finland (Traficom), Latvia (LGS), Luxembourg, Germany (DiPUL WFS and the DFS ED-318 service), France (IGN WFS), United Kingdom (NATS AIXM), Spain (ENAIRE), Netherlands (IenW), Austria (Austro Control) | Public downloads; the DFS ED-318 service needs a dipul account | Free; DiPUL CC BY-ND 4.0, others per publisher (see [Spain, Netherlands and Austria](#es-nl-at)) | ED-269 / ED-318 files and services, national GeoJSON, AIXM 5.1 | Native or close (restriction and limits per zone type) | Implemented: **Add Feed...**, **Download zones for the map area**, **Add Service...** |
| FAA (United States) | United States | Public, no account: ArcGIS feature services (UAS facility maps, national security UAS restrictions, class and special use airspace), `tfr.faa.gov` TFRs. NOTAM API (NMS): OAuth2 client credentials on request | US government data, public domain with a disclaimer (not for navigation); ArcGIS rate limit of 6000 request units a minute | GeoJSON of ArcGIS layers and of the TFR WFS, NOTAM details XML | Mapped per layer: facility map cells require authorisation up to their LAANC ceiling (0 ft prohibited), national security restrictions and prohibited areas are prohibited, controlled airspace and restricted areas require authorisation | Implemented (format `FaaAirspace`, `FaaTfr`); NMS NOTAMs configuration only |
| OpenAIP | Worldwide, community maintained | Free account and API key (`x-openaip-api-key` header or `apiKey` parameter), `GET /api/airspaces` with `bbox` (up to 5°), `page` and `limit`; daily per-country GeoJSON exports without a key | CC BY-NC 4.0: attribution, non-commercial use; rate limited | Airspace items: numeric `type` and `icaoClass`, `lowerLimit` / `upperLimit` (value, unit m/ft/FL, datum GND/MSL/STD), `hoursOfOperation`, GeoJSON polygon | Partial: conventional ICAO airspace, no UAS restriction or reasons. Mapped from the type (prohibited, restricted, CTR, danger areas...), class E–G and en-route airspace left out | Implemented as unofficial data (format `OpenAip`), never enabled by default: **Add Service...** (API key, by area) or the country export |
| Airspace Link (AirHub) | United States | OAuth2 client credentials and an `x-api-key` header; sandbox and production hosts; access through sales (free trial) | Commercial licence, content proprietary | `POST /v2/briefing/b4ufly/area` with a GeoJSON boundary answers notices: `dataSource`, `dataSubtype` (class B–E, prohibited, restricted...), `code`, `lowerAltitude` / `upperAltitude` (ft AGL), `schedule`, `contact`, optional geometry; vector tiles; LAANC operations | Good for authority, message, schedule and geometry; restriction and reasons derived from the notice codes; feet AGL only; a briefing rather than a zone feed | Configuration only (template **United States advisories (Airspace Link)**): needs a POST-geometry request and a notices parser, about 2–3 days once credentials are available |
| Unifly | EU and others: powers DFS dipul (Germany), skeyes DroneGuide (Belgium), NAV CANADA, ENAIRE | No public developer documentation; Operator and Platform APIs for partners | Enterprise agreements, not published | Not public | Can't be assessed | Not implemented. The official data it serves is available from the national feeds (DiPUL WFS, DFS ED-318) |
| Skylark | Unidentified. The only matching product is Skylark Drones' Watchtower (India): a free airspace map and paid APIs (tiles, advisories) with token authentication | No public API reference | Pay as you go | DGCA Digital Sky green / yellow / red zones | Unknown | Not implemented: confirm with Acecore which product the spec means |
| Altitude Angel | United Kingdom | GuardianUTM developer API (OAuth2) | — | — | — | Not integrated: the company entered administration on 7 October 2025, its developer site no longer resolves and its repositories are archived; Indra Group bought the GuardianUTM platform in January 2026 without a published continuation plan for API users. The spec also raises doubts about its data. |

### Spain, Netherlands and Austria {#es-nl-at}

Researched in November 2026 from the sites the customer named (drones.enaire.es, godrone.nl, utm.dronespace.at/avm), with the data downloaded and parsed:

| Country | Official machine-readable data | Format, size, updates | Licence / terms | In _QGroundControl_ |
| ------- | ------------------------------ | --------------------- | --------------- | ------------------- |
| Spain (ENAIRE) | ED-318 downloads on the AIP page [UAS geographical zones](https://aip.enaire.es/AIP/UAS-en.html): `https://aip.enaire.es/recursos/descargas/ZGUAS/ZGUAS_Aero.zip`, `ZGUAS_Urbano.zip`, `ZGUAS_Infra.zip` (with `_Metadata.txt` files). The servAIS data service (`https://servais.enaire.es/insigniads/rest/services/NSF_SRV/SRV_UAS_ZG_data_V2/FeatureServer`, layers 0 infrastructure, 2 aeronautical, 3 urban; also WFS 2.0 and WMS, documented in `servAIS_APIDOC.pdf`). The drones.enaire.es map is an ArcGIS Web AppBuilder application on other map services of ENAIRE | ED-318 GeoJSON in ENAIRE's variant (zone members in `UASZone`, limits in `verticalLayer`; the feature service flattens them into attributes, `f=geojson`, 15,000 features a request at most, pagination). Aero 2.5 MB zipped / 12.9 MB, 1,672 zones; Urbano 27 KB, 4 zones; Infra 18 MB zipped / 98 MB, 14,163 zones (mostly ADIF railways). Updated on AIRAC Thursdays (RD 517/2024) | ENAIRE's legal notice: use allowed when ENAIRE is credited as holder of the intellectual property; production service without 24/7 support, API versions overlap one AIRAC cycle | Feeds **Spain aeronautical zones (ENAIRE)** and **Spain urban zones (ENAIRE)**; the infrastructure zones by area (**Spain infrastructure zones (ENAIRE)**, pages of 2,000) since the whole file is near the 100 MB limit |
| Netherlands (IenW / LVNL) | The ED-269 file the ministry of Infrastructure and Water Management links from [luchtvaartindetoekomst.nl](https://www.luchtvaartindetoekomst.nl/onderwerpen/o/onbemande-luchtvaart) ("JSON-bestand", currently `https://www.nieuwsienw.nl/api/documents/downloadfile?sectionid=178144&fileid=1794622&forcedownload=true`, `UASZoneVersion_2026-01-01.json`) | ED-269 JSON, 873 KB, 743 zones (circles and polygons, m AGL), version of February 2026, updated a few times a year when the zoning regulation changes | CC0 1.0, as all content of luchtvaartindetoekomst.nl unless stated otherwise (its copyright page); the PDOK dataset was public domain | Feed **Netherlands (IenW)**: the page is read and the linked file downloaded (`datasetLinkPattern` without a date) |
| Netherlands, other | godrone.nl / map.godrone.nl is LVNL's GoDrone app built on Altitude Angel's platform: no public API. The PDOK _Drone no-fly Zones_ WFS/WMS/OGC API (`service.pdok.nl/lvnl/drone-no-flyzones`) was taken out of production on 1 July 2026. LVNL's geoportal (`geoportaal.lvnl.nl`) only has manned aviation airspace | — | — | Not used |
| Austria (Austro Control) | ED-269 datasets listed on [geografische Zonen](https://www.austrocontrol.at/luftfahrtbehoerde/lizenzen__bewilligungen/drohnen/geografische_zonen) with their validity, like `/jart/prj3/ac/releases/de/upload/Dronespace/UASZones-ACG-20261021-130632-full-production.zip` (valid from 25.10.2026) | ED-269 JSON (a list of zones without `UASZoneVersion`, with a byte order mark), 182 KB zipped / 1.5 MB, 288 zones; new versions every few weeks, published days before they take effect. Full messages in `extendedProperties.localizedMessages` (de-AT, en) | © Austro Control. No licence on the page; the site's imprint reserves all use of its contents to prior written permission. Each controller downloads the data from Austro Control itself (as for the EU 2019/947 publication the dataset is), but redistributing it, like in packs, should be cleared with Austro Control (dronespace@austrocontrol.at) | Feed **Austria (Austro Control)**: the page is read and the dataset in effect downloaded (`datasetLinkPattern` with the valid-from date, test datasets left out), refreshed daily |
| Austria, other | The Dronespace map utm.dronespace.at/avm (Frequentis/skyzr) loads `https://utm.dronespace.at/avm/utm/uas.geojson`: GeoJSON whose properties are ED-269 zones (3.5 MB, 283 zones with hidden system zones), live with temporary zones, but undocumented and for the web application | — | — | Not used: the official ED-269 publication is preferred |

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
