# LCD Community List — Geode starter mod

This starter mod is already connected to:

`https://api-lcd.vercel.app/levels`

## What it does right now

- Adds an **LCD** button to Geometry Dash's main menu.
- When tapped, it requests the live `/levels` API.
- Reads the `items` array.
- Sorts entries by `rank`.
- Shows the first 12 entries and the total count.

This is a starter/proof-of-connection UI. The next step is replacing the popup
with a proper scrollable list page, level buttons, thumbnails, etc.

## Automatic updates

The level data is NOT hard-coded in the mod.

When RankItem changes in Base44:
Base44 -> Vercel API -> mod fetches current data.

So you do not rebuild the mod just because the list changed.

## Mobile-friendly build using GitHub Actions

1. Create a new GitHub repository, for example `LCD-Geode-Mod`.
2. Upload every file/folder in this project.
3. Keep `.github/workflows/build.yml` exactly where it is.
4. Open the repo's **Actions** tab.
5. Open **Build Geode Mod**.
6. Tap **Run workflow**.
7. After the workflow finishes, open the run and download the
   `LCD-Community-List` artifact.

GitHub Actions performs the actual compilation, so you do not need to compile C++
on your phone.

## Important

This project targets Geode 5.8.2 / Geometry Dash 2.2081. If your installed Geode
or GD version differs later, update `mod.json` to the version you are developing
against.
