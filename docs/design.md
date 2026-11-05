# Design principles

CUI's look is deliberate. These are the rules every component follows, and they are a good guide for the screens you build with it.

## Color

- Most of the screen is neutral. The accent is spent with intent: the selected item, the one data series that matters, progress, focus.
- Status colors mean status. Green for success, amber for attention, red for failure, always with a label or an icon, never as decoration.
- Use semantic tokens, never raw colors, so every screen works in light and dark with any accent.

## Type

- Inter for interface text, Geist for large headings and numbers.
- Two weights only: regular and medium. Hierarchy comes from size, color and spacing.
- Sentence case for every label. Numbers that change or align use tabular figures.

## Surfaces

- Group by proximity first, a hairline border second, a card only for a real boundary.
- Cards rest on a border with no shadow. Only floating layers cast shadows: menus, popovers, dialogs, toasts.
- Corners are concentric: an inner radius is the outer radius minus the padding between them. Controls are pill shaped (18 pt), panels 22 pt, surfaces 28 pt.

## Spacing

- A 4 pt grid. 16 pt inside groups, 24 pt between regions, 32 to 48 pt between sections.
- Controls are 30, 36 or 44 pt tall.

## Motion

- Motion explains cause and effect: what opened, what changed, where it came from.
- Anything that reports state lands without overshoot (switch thumbs, progress, panel heights). Small feedback has a little life (presses, gliding highlights).
- Presses scale to about 0.97, except for triggers that anchor a menu: those answer with color.
- Exits are faster than entrances. Everything is interruptible because everything is a spring.
- Every animation has a reduced motion path that keeps the final state and the feedback.

## Interaction

- Every action works with the keyboard. Focus rings appear for keyboard focus, never for clicks.
- Give icon only buttons a tooltip: it is their name.
- Do not move layout under the user. Loading states keep their size; numbers keep their width.
- Prefer changing a control in place ("Saved") to a toast for the result of a button press. Toasts are for background events.

## What generic looks like

A lot of interface work now comes out of the same few templates, and people have learned to spot it. The tells are well catalogued ([Impeccable](https://www.impeccable.style/slop), [The Fountain Institute](https://www.thefountaininstitute.com/blog/signs-vibe-coded-ui), [this anti-pattern list](https://cdn.jsdelivr.net/npm/@wystack/agent-kit@0.12.0/wystack-agent-kit/references/anti-patterns.md), [and the case for coherence](https://dev.to/kiwibreaksme/why-ai-generated-uis-look-off-and-the-one-principle-that-fixes-it-4j20)). Almost none of them is wrong in isolation. They read as generic because they are defaults nobody chose. CUI's components and examples stay clear of them, and so should screens built with it.

**Color and light**

- No purple to blue or purple to pink gradients, no cyan on near black, no neon. The palette comes from the accent you pick, and the accent is used sparingly.
- No glow halos, colored blur blobs or spotlights behind content to make a screen feel atmospheric. Depth comes from surface levels and contrast. A glow is allowed only when it reports something: the voice orb's level, a pointer the user is moving.
- Headings and numbers are one solid color. Emphasis comes from size and weight, not gradient text. Gradient text exists as a component for the rare brand moment; it is never the default.
- Text on a colored surface is tinted toward that surface, never neutral gray on color.

**Surfaces and layout**

- No decorative grid or dot backgrounds. Grids are for canvases, maps and measuring.
- No cards inside cards. Inside a card, group with spacing, type and hairlines.
- An edge or a shadow, not both, for anything resting on the page. Floating layers (menus, popovers, dialogs) are the exception, because they sit over other content.
- No rows of identical icon tile, heading and blurb cards. If content is a list, it looks like a list.
- Left aligned by default. Center only what is truly alone: an empty state, a dialog's single message.
- Spacing has rhythm: tight inside a group, clearly looser between groups.

**Type and icons**

- No pill badge or eyebrow label above a heading to restate it. If a badge carries information, it sits next to the thing it describes.
- No icon in a rounded tile above every heading. An icon earns its place by helping someone find or recognize something.
- Sparkles mean nothing. CUI does not use them as a default icon for anything, and the AI components do not need them to say "AI".
- Monospace is for code and aligned technical data, not for looking technical.
- No emoji in place of icons.

**Status and motion**

- A dot means a state: online, unread, failed. No dots as decoration on tags or labels.
- Motion reports cause and effect. No pulsing on things that are not changing, no blinking caret on text nobody is typing, no bounce on routine dialogs.
- Marquees pause under the pointer and stop for reduced motion.

**Words and content**

- Say what something does. No "supercharge", "seamless", "world class", no slogans built from short fragments ("Fast. Small. Native."), no "Not X. Y." contrasts, no em dashes doing the work of a full stop.
- Button labels are a verb and an object: "Create project", not "Submit".
- Example content is specific and plausible. Real-looking names that are not famous people, prices and numbers that are not round templates, products with an actual job. Placeholder pioneers (Ada Lovelace, Grace Hopper) and template dashboards ("Revenue $15,231 +20.1%") are as generic as "John Doe".

**Coherence**

- One value per axis, used everywhere: one radius scale, one light direction, one accent, one icon set, one spacing scale, one set of motion curves. A mix of personalities on one screen reads as assembled rather than designed.
