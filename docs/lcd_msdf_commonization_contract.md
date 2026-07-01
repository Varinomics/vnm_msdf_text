# LCD/MSDF Commonization Contract

`vnm_msdf_text` exposes two dependency-light LCD/MSDF support components:

- `vnm_msdf_text::lcd_contract` provides the resolved LCD subpixel-order enum and
  constexpr mapping helpers in `<vnm_msdf_text/lcd_contract.h>`.
- `vnm_msdf_text::lcd_shader_reference` provides public-safe shader reference
  constants and GLSL literal fragments in
  `<vnm_msdf_text/lcd_shader_reference.h>`.

These components do not require FreeType or msdfgen. The full atlas-builder
target remains `vnm_msdf_text::vnm_msdf_text`; requesting the `atlas` component
imports that full target and requires the atlas dependencies. A no-component
`find_package(vnm_msdf_text CONFIG)` imports the full atlas target when the
install exported one. When an install only exports the dependency-light LCD
targets, no-component lookup still succeeds and leaves atlas consumers to
request `COMPONENTS atlas` explicitly.

## Resolved LCD Order

The resolved-order contract has no request state. It represents only the order
that is already resolved for rendering:

```cpp
namespace vnm::msdf_text::lcd {

enum class Resolved_lcd_subpixel_order : std::uint8_t
{
    NONE = 0,
    RGB  = 1,
    BGR  = 2,
    VRGB = 3,
    VBGR = 4,
};

constexpr bool is_display_specific(Resolved_lcd_subpixel_order order);
constexpr int resolved_order_value(Resolved_lcd_subpixel_order order);
constexpr float shader_uniform_value(Resolved_lcd_subpixel_order order);

}
```

`RGB`, `BGR`, `VRGB`, and `VBGR` are display-specific. `NONE` and invalid enum
casts fail closed: `is_display_specific` returns `false`,
`resolved_order_value` returns `0`, and `shader_uniform_value` returns `0.0f`.

Product request policy such as automatic detection remains consumer-owned.

## Shader Reference Data

The shader-reference component exposes reference data for consumer-owned drift
checks. It does not define a shared renderer or shader source.

The reference data includes:

- uniform values `0.0f` through `4.0f` matching the resolved C++ enum values;
- strict/open GLSL decode fragments `> 0.5 && < 1.5`,
  `> 1.5 && < 2.5`, `> 2.5 && < 3.5`, and `> 3.5 && < 4.5`;
- LCD filter weights `0.03125f`, `0.30078125f`, and `0.3359375f`;
- sample offsets `-3.0f` through `3.0f`;
- five filter weights for one channel reconstruction;
- three shifted five-tap filter windows named `first`, `center`, and `last`;
- subpixel divisor `3.0f` and step literals
  `1.0 / (3.0 * frame_size.x)` and `-1.0 / (3.0 * frame_size.y)`;
- opacity cutoff `0.999f` as reference data only.

The opacity cutoff is not a shared eligibility primitive. Consumers keep their
surface, backing, alpha, shadow, and renderer-resource eligibility policy.

## Package Components

Dependency-light consumers should request only the component they need:

```cmake
find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS lcd_contract)
target_link_libraries(app PRIVATE vnm_msdf_text::lcd_contract)
```

```cmake
find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS lcd_shader_reference)
target_link_libraries(check PRIVATE vnm_msdf_text::lcd_shader_reference)
```

Atlas consumers request the full component:

```cmake
find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS atlas)
target_link_libraries(renderer PRIVATE vnm_msdf_text::vnm_msdf_text)
```
