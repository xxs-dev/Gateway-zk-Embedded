#!/usr/bin/env python3
"""Regression checks for legacy BOOL conversion."""

from convert_comm_manager_xlsm import ModbusRow, VarRow, make_point, map_modbus_dtype


def main() -> None:
    assert map_modbus_dtype("BOOL") == ("bool", 1)
    source = ModbusRow(
        sheet="modbusRTU_1_Reg",
        row=4,
        device_name="微断",
        slave=1,
        address=12,
        rw_type="R",
        function=3,
        dtype="UINT16",
        scale=1.0,
        source_index=10,
        unit="",
        name="开关状态",
    )
    var = VarRow(
        row=4,
        mqtt_device_name="微断",
        meter_code="MTR_TEST",
        point_name="开关状态",
        point_code="KY02200011",
        rw_type="R",
        app_index=100,
        source_index=10,
        start_bit=0,
        data_len=16,
        dtype="BOOL",
        unit="",
    )

    point = make_point(source, var, 1, 500, read_source=source, write_source=source)
    assert point["read"]["dataType"] == "bool"
    assert point["read"]["scale"] == 1.0
    assert point["write"]["dataType"] == "bool"
    assert point["category"] == "status"
    assert point["reportOnChange"] is True
    assert set(point["tags"]) >= {"bool", "status", "writable"}

    frequency_source = ModbusRow(
        sheet="modbusRTU_1_Reg",
        row=5,
        device_name="微断",
        slave=1,
        address=6,
        rw_type="R",
        function=3,
        dtype="UINT16",
        scale=1.0,
        source_index=6,
        unit="",
        name="频率",
    )
    frequency_var = VarRow(
        row=5,
        mqtt_device_name="微断",
        meter_code="MTR_TEST",
        point_name="频率",
        point_code="KY02100095",
        rw_type="R",
        app_index=101,
        source_index=6,
        start_bit=0,
        data_len=16,
        dtype="UINT16",
        unit="",
    )
    frequency = make_point(
        frequency_source,
        frequency_var,
        1,
        500,
        read_source=frequency_source,
    )
    assert frequency["read"]["scale"] == 0.01
    assert frequency["read"]["unit"] == "Hz"


if __name__ == "__main__":
    main()
