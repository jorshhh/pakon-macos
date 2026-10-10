# Website

The project site runs on [Ghost](https://ghost.org): the homepage is a
one-screen Mac download page and the blog lives at `/blog/`.

- `theme/pakon/`: the Ghost theme, in the same retro Kodak minilab style as
  the web UI. `home.hbs` is the landing page, `index.hbs` the blog.
- `routes.yaml`: puts the landing page at `/` and the blog at `/blog/`.
- `docker-compose.yml`: a local Ghost for working on the theme.

## Run it locally

```sh
cd website
docker compose up -d
```

Open <http://localhost:2368/ghost>, create the admin account, then:

1. **Settings → Design & branding → Change theme**: activate **pakon**
   (it is mounted from `theme/pakon`, so edits show up on reload).
2. **Settings → Labs → Routes → Upload routes file**: upload `routes.yaml`.
3. **Settings → Navigation**: set the primary links, e.g. *Blog* `/blog/`.
   The header adds the GitHub link on its own.

Without step 2 the homepage still works, but the blog has no `/blog/` page.

## Theme settings

In **Settings → Design & branding → Site design**:

| Setting        | Default |
|----------------|---------|
| Download URL   | `https://github.com/jorshhh/pakon-macos/releases/latest/download/Pakon.dmg` |
| Repo URL       | `https://github.com/jorshhh/pakon-macos` |
| Version label  | `Free and open source · macOS` |

**Download for Mac** links straight to a `Pakon.dmg` asset on the latest
GitHub release, so each new release updates the button with no site change.
Until a release with that file exists, the link returns 404.

## Check and package the theme

```sh
cd website/theme/pakon
npm test        # gscan, Ghost's theme validator
npm run zip     # writes website/dist/pakon-theme.zip
```

Upload the zip in **Settings → Design & branding → Change theme → Upload
theme** on the production Ghost (Ghost(Pro) or a self-hosted install).
