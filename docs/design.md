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
