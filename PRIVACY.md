# Privacy Policy — Trackday Pyrometer Helper

**Effective date:**  June 15, 2026
**App:** Trackday Pyrometer Helper (Android)
**Developer:** Matthew Ryan
**Contact:** matt@geekopolis.com

This policy explains what Trackday Pyrometer Helper (the "App") does with information. Please read it alongside the App's Google Play "Data safety" section.

## Summary

- The App stores your data **on your device only** (car profiles, tire temperatures, pressures, corner weights, scale calibrations, TPMS sensor sets, packing checklists, and your theme choice).
- We, the developer, operate **no servers** and **do not collect, receive, sell, or share** any of your data.
- There are **no user accounts, no analytics, no advertising, and no third‑party tracking SDKs**.
- The **only** information that leaves your device is your **approximate location (latitude/longitude)**, which is sent to the **Open‑Meteo** weather service to look up the current outside temperature (used to detect whether your tires are cold). This happens only when an ambient‑temperature lookup is performed.
- Bluetooth is used to talk to your own measurement hardware (pyrometer, corner scales, tire‑pressure sensors). That data stays on your device.

## Information the App handles

### Data stored on your device
The App saves the following locally using Android's standard app storage. It is not transmitted to us:

- Vehicle/car profiles and their settings (weights, camber, tire targets, sessions).
- Tire temperature readings, tire pressures, and corner‑weight values.
- Corner‑scale device mappings and calibration values.
- Saved tire‑pressure (TPMS) sensor sets.
- Packing checklists.
- App preferences (such as the Daylight/Dark theme).

This data remains on your device unless you choose to back it up or export it (see **Backups and exports** below).

### Bluetooth
The App uses Bluetooth Low Energy to discover and connect to nearby measurement devices that you own — for example a tire pyrometer, wireless corner‑weight scale pads, and tire‑pressure sensors. Readings received over Bluetooth (temperatures, weights, pressures) are processed and stored **on your device**. We do not receive this data.

On Android 12 and newer, the App declares the Bluetooth scan permission with the `neverForLocation` flag, which tells the system the App does **not** use Bluetooth scanning to derive your physical location.

### Location
The App uses your device's **approximate, last‑known location** for one purpose: to look up the current local outside (ambient) temperature so it can flag cold tires. It reads the last‑known location from the system; it does not continuously track or log your movements, and it does not store your location.

On Android 11 and older, the App also requests the fine‑location permission **only because older versions of Android require a location permission in order to scan for Bluetooth devices at all**. It is not used to determine or record your whereabouts.

### Weather lookup (the only network request)
When an ambient‑temperature lookup runs, the App sends your **approximate latitude and longitude** to **Open‑Meteo** (`https://api.open-meteo.com`) over HTTPS and receives back the current temperature for that area. No API key, account, name, device identifier, or other personal information is sent with this request. Your coordinates are used by Open‑Meteo to fulfill the request and are governed by Open‑Meteo's own privacy terms: https://open-meteo.com/en/terms

If you do not grant location permission, or there is no network connection, this feature simply does not run — the rest of the App continues to work.

## Information we collect

**None.** The developer does not collect, store, or have access to any of your information. The App has no analytics, no crash‑reporting service, no advertising, and no third‑party tracking or marketing libraries.

## Backups and exports

You control all copies of your data:

- **Manual export:** You can export your App data to a single JSON file and save it to a location of your choosing (for example Google Drive or your device's Downloads). That file contains your App data and is controlled entirely by you. We never receive it.
- **Android Auto Backup:** Android may automatically back up the App's data to **your own Google account/Google Drive**, and restore it on a new device or reinstall. This is a standard Android feature operated by Google under your Google account; the developer does not have access to those backups. Google's handling of that data is governed by Google's Privacy Policy: https://policies.google.com/privacy. You can disable backup in your device's system settings.

## Third‑party services

- **Open‑Meteo** — used to retrieve current outside temperature from your approximate coordinates. Terms: https://open-meteo.com/en/terms
- **Google (Android Auto Backup / Google Drive)** — only if you have backups enabled on your device; data is stored under your Google account. Privacy Policy: https://policies.google.com/privacy

We have no other third‑party data‑sharing relationships.

## Permissions and why they are used

| Permission | Why the App uses it |
|---|---|
| Bluetooth scan / connect (Android 12+) | Find and connect to your measurement devices (pyrometer, scales, TPMS sensors). Declared `neverForLocation`. |
| Bluetooth / Bluetooth Admin (Android 11 and below) | The legacy permissions required to use Bluetooth on older Android versions. |
| Approximate location (`ACCESS_COARSE_LOCATION`) | Get last‑known location to look up local ambient temperature for cold‑tire detection. |
| Precise location (`ACCESS_FINE_LOCATION`, Android 11 and below only) | Required by older Android versions to permit Bluetooth scanning. Not used to track you. |
| Internet | Make the single HTTPS request to Open‑Meteo for the ambient temperature. |

## Data retention and deletion

- Your data stays on your device until you delete it in the App or **uninstall** the App, which removes the App's locally stored data.
- To delete an exported backup file, delete it from wherever you saved it (e.g. Google Drive, Downloads).
- To delete data held in Android Auto Backup, use your Google account/Drive controls or disable backup in your device settings.

Because we do not collect or store your data, there is nothing held by us to request access to or deletion of.

## Children

The App is a tool for motorsport/automotive use and is not directed to children. We do not knowingly collect any information from children.

## Security

Your data is stored using Android's standard app‑private storage. The single network request (to Open‑Meteo) is made over HTTPS. Because no personal data is sent to or held by us, the risk surface is limited to your own device and any backups you choose to create.

## Changes to this policy

We may update this policy from time to time. Material changes will be reflected by updating the "Effective date" above and posting the new version at this URL. Continued use of the App after an update constitutes acceptance of the revised policy.

## Contact

Questions about this policy can be sent to: matt@geekopolis.com
