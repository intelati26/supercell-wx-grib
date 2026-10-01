# Supercell Wx Features

Supercell Wx is a cross-platform weather radar and severe-weather visualization
application. It combines live and archived NEXRAD data with alerts and other
weather overlays on an interactive map.

## Radar Data

- View live and archived NEXRAD Level 2 and Level 3 data.
- Select a radar site and browse its available products and volume times.
- Automatically refresh products as new radar scans become available.
- Load radar data from supported network providers or from local files.
- Cache loaded products to make time navigation and repeat viewing faster.
- Display multiple radar products simultaneously as map layers.
- Select among available elevation tilts for Level 2 products.
- Smooth radar data when appropriate for the selected product.
- Adjust radar product opacity and configure product defaults.

### Supported Product Families

Level 2 moments include:

- Reflectivity
- Velocity
- Spectrum width
- Differential reflectivity
- Differential phase
- Correlation coefficient
- Clutter-filter power removed

Level 3 categories include:

- Reflectivity and digital reflectivity
- Velocity and storm-relative velocity
- Spectrum width
- Differential reflectivity
- Specific differential phase
- Correlation coefficient
- Vertically integrated liquid
- Echo tops
- Hydrometeor classification
- Precipitation and storm-total accumulation products

The exact product list depends on the selected radar site and the data provider.

## Time Navigation and Animation

- Switch between live time and archived time views.
- Select a radar volume or product time from the timeline.
- Step backward and forward through available scans.
- Play, pause, and control animated radar loops.
- Configure loop duration, playback speed, and loop delay.
- Keep multiple map panes synchronized during animation.
- Prefetch data needed by an archive loop where supported.

## Map and Workspace

- Render radar data over a responsive, GPU-accelerated map.
- Choose and configure the map style through MapTiler or Mapbox providers.
- Display radar sites and radar range rings.
- Use multiple map panes for side-by-side comparison.
- Link map pane navigation and time state.
- Pop a map pane out into a separate window.
- Save and restore map pane layouts.
- Configure map layers and map-specific display settings.
- Match new panes to the first pane's map style or keep per-pane styles.
- Inspect map values and coordinates with map interaction tools.

## Alerts and Weather Information

- Display severe weather alerts on the map.
- Parse NWS/AWIPS text products and alert messages.
- Open archived text event product files.
- Associate alert and text-event messages with their valid times.
- Play configurable audio notifications for supported severe-weather phenomena.
- Display the WPC Day 1 Excessive Rainfall Outlook when its data is available.

## Overlays and Supplemental Data

- Load and render user-configured GR placefiles.
- Enable, disable, refresh, and threshold placefiles.
- Persist placefile URLs, titles, categories, and display state.
- Load placefile fonts and image resources.
- Display weather outlooks through the placefile layer.
- Display wind barbs from supported RTMA model data.
- View supported MRMS and model GRIB2 products, including live and archive
  frames.
- Switch among curated supplemental-data products at runtime.
- Prefetch supplemental-data frames needed by an animation loop.

Supplemental products and providers are dependent on the current build and
available network data.

## Annotations and Locations

- Create and manage persistent location markers.
- Customize marker names, icons, and colors.
- Draw and manage map annotations.
- Display radar site metadata and status information.

## Controls and Preferences

- Configure map, radar product, alert, text, audio, unit, palette, and interface
  settings.
- Configure keyboard hotkeys.
- Choose display units for supported physical quantities.
- Configure radar and overlay color palettes.
- Configure fonts, line styles, and button behavior.
- Check for application updates.
- Export supported application settings.
- View diagnostic logging and loaded radar product records.

## Platform Support

Supercell Wx supports 64-bit Windows, Linux, and macOS builds. The exact
supported operating-system versions and graphics requirements are listed in
the main [README](README.md).

## Implementation Scope

This feature list describes functionality implemented in the application and
its first-party data library. Availability of live data, map tiles, alerts,
placefiles, and supplemental model products depends on provider availability,
network access, API keys, and the selected radar site.