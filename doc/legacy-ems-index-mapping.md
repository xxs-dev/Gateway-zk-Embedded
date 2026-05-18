# Legacy EMS Index Mapping

## Averaging and Derived Points

| Legacy Input | Meaning | Current Output |
| --- | --- | --- |
| `1030..1043` | TQ meter base values | `201..225` |
| `1130..1143` | CN meter base values | `251..266` |
| `4536..4543` | Legacy BW/settlement meter base values before address migration | migrated to `4636..4643`; internal BW averages, then `309..325` via `FH=TQ-BW` |
| `TQ + CN` | load derived | `309..325` via `FH=TQ-CN` |

## BMS Derived

| Legacy Input | Meaning | Current Output |
| --- | --- | --- |
| `1566 + 1556` | charge voltage/current allow | `1552` |
| `1566 + 1557` | discharge voltage/current allow | `1553` |
| `1586 - 398` | charge kWh today | `1615` |
| `1587 - 399` | discharge kWh today | `1616` |

## COS Mode

| Legacy Input | Meaning | Current Output |
| --- | --- | --- |
| `514` | target COS | `505..508` target reactive power |
| TQ reactive deviation | compensation output | `601..604` |
| compensation active | run flag | `8` |

## LV/HV Mode

| Legacy Input | Meaning | Current Output |
| --- | --- | --- |
| `544/545` | LV low/high | `605..608`, `10` |
| `546/547` | HV low/high | `609..612`, `12` |
| `533` | `Grad_P` | used by LV/HV step output |
| `535` | `PCS_P1_MAX` | used by LV/HV clamp |

## Charge/Discharge Mode

| Legacy Input | Meaning | Current Output |
| --- | --- | --- |
| `451/452` | charge target P/SOC | `613`, `14` |
| `455/456` | discharge target P/SOC | `614`, `16` |
| `453/454/457/458` | TQ limit control inputs | affects `613/614` |
| `1570` | stack SOC | affects `613/614`, `14/16` |

## DS / GF / PH / SK

| Legacy Input | Meaning | Current Output |
| --- | --- | --- |
| `400..423` | hourly DS power schedule | current hour drives `461`, `615..618`, `18` |
| `424..447` | hourly DS SOC schedule | current hour drives `462`, `615..618`, `18` |
| `760..783` | hourly DS mode schedule | current hour drives `615..618`, `18` |
| `463/464` | DS voltage thresholds | affects `615..618` |
| `581/583` | GF charge time window | `619..622`, `22` |
| `562` | PH allowed unbalance percent | `564..567`, `623..625`, `20` |
| `23/588` | ZR enable and reserve active power | `24`, then constrains `627..629` |
| `590/591` | SK active/reactive total setpoint | `26`, then overrides `627..632` |

## Power Solve

| Legacy Input | Meaning | Current Output |
| --- | --- | --- |
| `615..617` | DS active outputs | `627..629` base outputs |
| `601..603` | reactive compensation outputs | `630..632` |
| `605..607` | LV active outputs | `627..629` merge candidates |
| `609..611` | HV active outputs | `627..629` merge candidates |
| `619..621` | GF active outputs | `627..629` merge candidates |
| `623..625` | PH active offsets | added onto `627..629` |
| `23/588 + 457 + FH` | ZR reserve constraint | clamps `627..629` more negative |
| `454/453 + FH` | charge residual limit | post-limits positive `627..629` |
| `458/457 + FH` | discharge residual limit | post-limits negative `627..629` |
| `590/591 + 26` | SK total override | overrides `627..632` |
| `535/504/151` | P/Q/S limits | clamps final `627..632` |
| `1552/1553` | BMS charge/discharge total kW allow | scales final `627..629` by total active sum |
| `1570/161/162` | BMS SOC and high/low limits | blocks charge/discharge side of final `627..632`, clears affected run flags |

## PCS Writeback

| Legacy Input | Meaning | Current Output |
| --- | --- | --- |
| `1399` | PCS communication status | enables command submission only when value is `1` |
| `627..632` | final PCS P/Q outputs | submits business-value write commands to `1318..1323` |
| `PCS_MODEL=3` | Shenghong PCS scaling | legacy targets are integer-truncated business values; device register scaling stays in point `write.scale` |

## Verification Notes

- DS and GF use the host local hour, matching legacy `GetSysItem(3)` semantics instead of UTC epoch hour modulo 24.
- Non-zero DS schedules are covered by a regression test for local hour 5, including `405/429/765 -> 461/18`.
- Address migration fixes the old overlap explicitly: `4500..4599` is reserved for LED display/control, and settlement/BW meter points move to the `4600..4699` segment. Legacy BW points `4536..4543/4599` should be mapped as `4636..4643/4699` in new examples and platform-generated point tables.
