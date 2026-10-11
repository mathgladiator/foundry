# Third-party services

This document lists outside services that can help produce assets, starting with Meshy, which is already available. Prices, model versions, and terms in this space change every few months; everything here was checked in October 2026, and anything marked *unverified* comes from third-party reviews rather than the vendor.

## Ground rules

- **Build time only.** Third-party services run in the foundry. Their output is baked into disk formats (`flow.md`), and the game never calls them.
- **Output is raw material.** Generated meshes, images, and sounds are references and starting points. The pipelines, the style layer, and the palettes (`style.md`) turn them into house style.
- **Record provenance.** Every generated file gets a JSON sidecar with the service, model version, prompt, inputs, cost, date, and license, as in `meshy/concepts/*.png` (`flow.md`). The workspace tool shows these, and they make it possible to answer "where did this come from, and can we ship it?"
- **Check the license before shipping.** Terms differ by service and by plan, and free tiers often restrict commercial use.

## Meshy

Meshy is an AI 3D generator with a web app, a REST API, and an MCP server.

### What it does

| Feature | What it gives us | Credits (API) |
|---|---|---|
| Text to image | Concept art for approval before any 3D work | 3–9 |
| Image to image | Variations on an approved concept | 3–12 |
| Text to 3D | A mesh from a prompt | 20 |
| Image to 3D | A mesh from an approved concept image | 20 (30 with texture, 35 with 8K texture) |
| Multi-image to 3D | A mesh from several views (front, side, back) for better accuracy | 20 |
| Texturing / retexture | New textures on an existing mesh, from a prompt | 10 (2K or 4K), 15 (8K) |
| Remesh | Clean topology at a lower face count | 5 |
| Auto-rigging | A skeleton and skin weights for a humanoid or quadruped | 5 |
| Animation | Preset animations applied to a rigged model | 3 |
| Convert / resize | File format conversion and scaling | 1 each |

The current models are **Meshy 7** (2026, most detail), **Meshy 6** (2025, stable structure), and **Smart Topology** (2026, aimed at games and real-time rendering, around 10 seconds per model). Older models cost fewer credits.

Exports: GLB, FBX, OBJ, USDZ, STL, and 3MF. GLB (textures embedded) is the simplest for the foundry to import.

### API limits

- API access requires the **Pro** plan or higher. The free plan gives 100 credits a month in the web app.
- Rate limits are 20 requests per second on every paid plan, with a queue of 10 tasks (Pro) up to 100 (Ultra). Over the limit returns HTTP 429.
- **Generated models are deleted after 3 days** on non-Enterprise plans. The foundry must download every result immediately.
- Meshy states that API inputs and outputs are not used for training.

### Rigging limits

Auto-rigging handles humanoids and quadrupeds only; props, buildings, and vehicles can't be rigged. Meshy recommends remeshing before rigging, because dense or uneven topology causes bad skin weights, and inputs near a T-pose or A-pose rig best. Unusual body plans (a naga, an octopod) need manual rigging, which in the foundry means our own rigging system (`systems.md`).

### How Meshy fits the foundry

- **Concepts first.** Generate concept images (text to image), approve them, then go to 3D with image to 3D or multi-image to 3D. Concepts are cheap; 3D is not.
- **Static things** (`systems.md`) are the most direct use: generate, remesh, resize to the conformance shape, bake.
- **Reference studies.** For the humanoid body mesh and creature bodies, Meshy output is a reference for structure, not the final mesh.
- **Baseline animations.** Rigging plus animation gives the humanoid pipeline a starting point to compare against.
- **Particle shapes** for magic (`systems.md`), flattened to the effect's palette afterward.
- **Use the house prompts.** `style.md` §8 has the shared style, texture, staging, and pose blocks. The texture block matters most: Meshy tends to bake lighting into textures, which fights the cel shading.
- **Use the MCP server.** Meshy's MCP server could let the foundry's agent call Meshy directly, with each call recorded in a provenance sidecar.

## Other 3D generators

Reviews disagree about which is best, and agree that none is a one-click production tool for deforming characters (*unverified*, from third-party comparisons).

| Service | Strength | Notes |
|---|---|---|
| Tripo | Fast; good at stylized assets; auto-rigging | Dense output may need decimation and retopology |
| Rodin (Hyper3D) | Highest detail, especially hard-surface props | Most expensive; very high polygon counts need decimation |
| Hunyuan3D (Tencent) | Open source; can run locally | Usually needs cleanup before production |
| TRELLIS.2 (Microsoft) | Open source; strong visual quality; runs locally in ComfyUI | Needs a capable local GPU |

The open-source options are worth keeping in mind for bulk work, since local generation has no per-model cost.

## Animation

- **Mixamo (Adobe).** Free humanoid auto-rigging and a large animation library. Humanoid only. (*unverified* for current availability and terms)
- **Cascadeur.** Animation tool with physics assistance, useful for making keyframe animation look physically plausible.
- **Video motion capture** (Move.ai, DeepMotion, Rokoko Vision): turn a video of a person into animation. Useful for reference motions. (*unverified*)

## Images and textures

- **Concept art:** Meshy's text to image keeps everything in one place. Local Stable Diffusion or Flux models in ComfyUI cost nothing per image.
- **Textures:** Poly Haven and ambientCG offer free (CC0) textures. With flat palettes (`style.md` §5), we should rarely need them.

## Sound

Sounds are tables of pre-produced files (`sounds.md`), so these services are used at build time to fill the tables.

- **ElevenLabs.** Generates sound effects from a text prompt (good for short one-shots: impacts, footsteps, UI clicks) and has a sound effects API. It is also widely used for synthetic voice in many languages, which fits the voice and language dimensions in `sounds.md`. (*unverified*: current pricing, voice features, and game licensing terms; check the vendor's docs)
- **Stable Audio.** Reported to be stronger on ambient beds and longer clips, which suits loops. (*unverified*)
- **Freesound.** A large library of recorded sounds. Each file has its own Creative Commons license, so record it in the sidecar.
- **Sonniss GDC bundles.** Large royalty-free sound effect packs released yearly.

## Libraries (not services)

These are open-source tools the host can use directly:

- **meshoptimizer.** Simplification for LODs and vertex cache, overdraw, and fetch optimization (`submit.md`).
- **Blender**, run headless from scripts, for format conversion, decimation, and inspection of third-party meshes.

## Sources

- [Meshy documentation](https://docs.meshy.ai/en)
- [Meshy API page (credits, rate limits, retention)](https://www.meshy.ai/api)
- [Meshy rigging guide](https://docs.meshy.ai/en/webapp/guides/3d-model/rigging)
- [Meshy animation guide](https://docs.meshy.ai/en/webapp/guides/animate)
- [AI 3D model generators for game development (2026)](https://wavect.io/blog/ai-3d-model-generators-game-development-2026/)
- [Best AI 3D model generators in 2026: TRELLIS vs Meshy vs Tripo](https://trellis2.app/blog/best-ai-3d-model-generator)
- [Best AI for 3D models 2026: Meshy vs Tripo vs Rodin](https://coruzant.com/ai/best-ai-for-3d-models-honest-comparison/)
- [Best AI sound effect generators for games in 2026](https://app.cinevva.com/guides/ai-sound-effect-generators)
