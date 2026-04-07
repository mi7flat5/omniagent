"""Integration tests for Phase 2: Dispatch Optimizer.

Tests API wiring and cross-module calls without validating calculation correctness.
"""

import sys
import os
from datetime import datetime

# Add project root to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'projects', 'gridbattery'))

from gridbattery_core.constants import CHEMISTRY_SPECS, get_chemistry_spec, get_available_chemistries
from gridbattery_core.schema import (
    BatterySpec,
    EnergyPrices,
    AncillaryPrices,
    CapacityMarket,
    DispatchYear,
    HourlyDispatch,
)
from gridbattery_core.dispatch import DispatchOptimizer, DispatchResult


def test_import_constants_module():
    """Test that constants module imports correctly."""
    assert CHEMISTRY_SPECS is not None
    assert get_chemistry_spec is not None
    assert get_available_chemistries is not None


def test_import_schema_module():
    """Test that schema module imports correctly."""
    assert BatterySpec is not None
    assert EnergyPrices is not None
    assert AncillaryPrices is not None
    assert CapacityMarket is not None
    assert DispatchYear is not None
    assert HourlyDispatch is not None


def test_import_dispatch_module():
    """Test that dispatch module imports correctly."""
    assert DispatchOptimizer is not None
    assert DispatchResult is not None


def test_get_chemistry_spec():
    """Test chemistry spec lookup."""
    spec = get_chemistry_spec("LFP")
    assert spec is not None
    assert spec.chemistry == "LFP"
    assert spec.roundtrip_efficiency == 0.87
    assert spec.cost_per_kw == 350


def test_get_available_chemistries():
    """Test getting available chemistries."""
    chemistries = get_available_chemistries()
    assert isinstance(chemistries, list)
    assert "LFP" in chemistries
    assert "NMC" in chemistries


def test_battery_spec_creation():
    """Test creating a BatterySpec instance."""
    battery = BatterySpec(
        chemistry="LFP",
        power_mw=10.0,
        energy_mwh=20.0,
        duration_hours=2.0,
        roundtrip_efficiency=0.87,
        depth_of_discharge=0.90,
        min_soc=0.10,
        max_soc=1.00,
        max_cycles_per_day=2,
        calendar_life_years=20,
        cycle_life_cycles=6000,
        annual_degradation_pct=2.0,
        augmentation_year=10,
        augmentation_cost_pct=30,
        cost_per_kw=350,
        cost_per_kwh=250,
        epc_cost_pct=15,
        interconnection_cost=500000,
        annual_om_per_kw=10,
        variable_om_per_mwh=2.50,
        warranty_pct=0.009,
        warranty_start_year=3,
        capacity_credit_pct=0.95,
        capacity_price_per_kw_yr=50,
        freq_reg_capable=True,
        spinning_reserve_capable=True,
        black_start_capable=False,
        annual_insurance_pct=0.004,
        annual_property_tax_pct=0.0084,
        annual_land_lease=50000,
        decommissioning_cost_pct=0.05,
    )
    assert battery is not None
    assert battery.power_mw == 10.0
    assert battery.energy_mwh == 20.0


def test_energy_prices_creation():
    """Test creating EnergyPrices instance."""
    timestamps = [f"2024-01-01T{h:02d}:00:00" for h in range(24)]
    prices = [float(i) for i in range(24)]
    
    energy_prices = EnergyPrices(
        timestamps=timestamps,
        price_per_mwh=prices,
        year=2024,
        escalation_rate_pct=2.0,
    )
    assert energy_prices is not None
    assert len(energy_prices.timestamps) == 24
    assert len(energy_prices.price_per_mwh) == 24


def test_ancillary_prices_creation():
    """Test creating AncillaryPrices instance."""
    timestamps = [f"2024-01-01T{h:02d}:00:00" for h in range(24)]
    freq_reg = [float(i) for i in range(24)]
    spinning_reserve = [float(i * 0.5) for i in range(24)]
    non_spinning_reserve = [float(i * 0.25) for i in range(24)]
    
    ancillary_prices = AncillaryPrices(
        timestamps=timestamps,
        freq_reg_price=freq_reg,
        spinning_reserve_price=spinning_reserve,
        non_spinning_reserve_price=non_spinning_reserve,
        year=2024,
        reg_energy_exclusive=True,
    )
    assert ancillary_prices is not None
    assert len(ancillary_prices.freq_reg_price) == 24


def test_capacity_market_creation():
    """Test creating CapacityMarket instance."""
    capacity_market = CapacityMarket(
        capacity_price_per_kw_yr=50,
        min_duration_hours=4,
        elcc_method="marginal",
        delivery_year_lead=3,
    )
    assert capacity_market is not None
    assert capacity_market.capacity_price_per_kw_yr == 50


def test_dispatch_optimizer_initialization():
    """Test DispatchOptimizer initialization."""
    battery = BatterySpec(
        chemistry="LFP",
        power_mw=10.0,
        energy_mwh=20.0,
        duration_hours=2.0,
        roundtrip_efficiency=0.87,
        depth_of_discharge=0.90,
        min_soc=0.10,
        max_soc=1.00,
        max_cycles_per_day=2,
        calendar_life_years=20,
        cycle_life_cycles=6000,
        annual_degradation_pct=2.0,
        augmentation_year=10,
        augmentation_cost_pct=30,
        cost_per_kw=350,
        cost_per_kwh=250,
        epc_cost_pct=15,
        interconnection_cost=500000,
        annual_om_per_kw=10,
        variable_om_per_mwh=2.50,
        warranty_pct=0.009,
        warranty_start_year=3,
        capacity_credit_pct=0.95,
        capacity_price_per_kw_yr=50,
        freq_reg_capable=True,
        spinning_reserve_capable=True,
        black_start_capable=False,
        annual_insurance_pct=0.004,
        annual_property_tax_pct=0.0084,
        annual_land_lease=50000,
        decommissioning_cost_pct=0.05,
    )
    
    optimizer = DispatchOptimizer(battery)
    assert optimizer is not None
    assert optimizer.battery is not None
    assert optimizer.efficiency == 0.87
    assert optimizer.power_limit == 10.0
    assert optimizer.energy_limit == 20.0


def test_dispatch_optimizer_optimize_with_minimal_inputs():
    """Test DispatchOptimizer.optimize with minimal valid inputs."""
    battery = BatterySpec(
        chemistry="LFP",
        power_mw=10.0,
        energy_mwh=20.0,
        duration_hours=2.0,
        roundtrip_efficiency=0.87,
        depth_of_discharge=0.90,
        min_soc=0.10,
        max_soc=1.00,
        max_cycles_per_day=2,
        calendar_life_years=20,
        cycle_life_cycles=6000,
        annual_degradation_pct=2.0,
        augmentation_year=10,
        augmentation_cost_pct=30,
        cost_per_kw=350,
        cost_per_kwh=250,
        epc_cost_pct=15,
        interconnection_cost=500000,
        annual_om_per_kw=10,
        variable_om_per_mwh=2.50,
        warranty_pct=0.009,
        warranty_start_year=3,
        capacity_credit_pct=0.95,
        capacity_price_per_kw_yr=50,
        freq_reg_capable=True,
        spinning_reserve_capable=True,
        black_start_capable=False,
        annual_insurance_pct=0.004,
        annual_property_tax_pct=0.0084,
        annual_land_lease=50000,
        decommissioning_cost_pct=0.05,
    )
    
    optimizer = DispatchOptimizer(battery)
    
    # Create minimal energy prices (24 hours)
    timestamps = [f"2024-01-01T{h:02d}:00:00" for h in range(24)]
    prices = [float(i) for i in range(24)]
    energy_prices = EnergyPrices(
        timestamps=timestamps,
        price_per_mwh=prices,
        year=2024,
        escalation_rate_pct=2.0,
    )
    
    # Run optimization
    result = optimizer.optimize(
        energy_prices=energy_prices,
        ancillary_prices=None,
        capacity_market=None,
        year=2024,
    )
    
    # Verify return type
    assert isinstance(result, DispatchYear)
    assert result.year == 2024
    assert isinstance(result.hours, list)
    assert len(result.hours) == 24
    assert isinstance(result.total_discharge_mwh, float)
    assert isinstance(result.total_charge_mwh, float)
    assert isinstance(result.equivalent_cycles, float)
    assert isinstance(result.total_arbitrage_revenue, float)
    assert isinstance(result.total_charging_cost, float)
    assert isinstance(result.peak_load_mw, float)
    assert isinstance(result.peak_overload_mw, float)
    assert isinstance(result.capacity_factor_pct, float)


def test_dispatch_optimizer_optimize_with_ancillary_prices():
    """Test DispatchOptimizer.optimize with ancillary prices."""
    battery = BatterySpec(
        chemistry="LFP",
        power_mw=10.0,
        energy_mwh=20.0,
        duration_hours=2.0,
        roundtrip_efficiency=0.87,
        depth_of_discharge=0.90,
        min_soc=0.10,
        max_soc=1.00,
        max_cycles_per_day=2,
        calendar_life_years=20,
        cycle_life_cycles=6000,
        annual_degradation_pct=2.0,
        augmentation_year=10,
        augmentation_cost_pct=30,
        cost_per_kw=350,
        cost_per_kwh=250,
        epc_cost_pct=15,
        interconnection_cost=500000,
        annual_om_per_kw=10,
        variable_om_per_mwh=2.50,
        warranty_pct=0.009,
        warranty_start_year=3,
        capacity_credit_pct=0.95,
        capacity_price_per_kw_yr=50,
        freq_reg_capable=True,
        spinning_reserve_capable=True,
        black_start_capable=False,
        annual_insurance_pct=0.004,
        annual_property_tax_pct=0.0084,
        annual_land_lease=50000,
        decommissioning_cost_pct=0.05,
    )
    
    optimizer = DispatchOptimizer(battery)
    
    # Create minimal energy prices
    timestamps = [f"2024-01-01T{h:02d}:00:00" for h in range(24)]
    prices = [float(i) for i in range(24)]
    energy_prices = EnergyPrices(
        timestamps=timestamps,
        price_per_mwh=prices,
        year=2024,
        escalation_rate_pct=2.0,
    )
    
    # Create ancillary prices
    freq_reg = [float(i) for i in range(24)]
    spinning_reserve = [float(i * 0.5) for i in range(24)]
    non_spinning_reserve = [float(i * 0.25) for i in range(24)]
    
    ancillary_prices = AncillaryPrices(
        timestamps=timestamps,
        freq_reg_price=freq_reg,
        spinning_reserve_price=spinning_reserve,
        non_spinning_reserve_price=non_spinning_reserve,
        year=2024,
        reg_energy_exclusive=True,
    )
    
    # Run optimization
    result = optimizer.optimize(
        energy_prices=energy_prices,
        ancillary_prices=ancillary_prices,
        capacity_market=None,
        year=2024,
    )
    
    assert isinstance(result, DispatchYear)
    assert len(result.hours) == 24


def test_dispatch_optimizer_optimize_with_capacity_market():
    """Test DispatchOptimizer.optimize with capacity market."""
    battery = BatterySpec(
        chemistry="LFP",
        power_mw=10.0,
        energy_mwh=20.0,
        duration_hours=2.0,
        roundtrip_efficiency=0.87,
        depth_of_discharge=0.90,
        min_soc=0.10,
        max_soc=1.00,
        max_cycles_per_day=2,
        calendar_life_years=20,
        cycle_life_cycles=6000,
        annual_degradation_pct=2.0,
        augmentation_year=10,
        augmentation_cost_pct=30,
        cost_per_kw=350,
        cost_per_kwh=250,
        epc_cost_pct=15,
        interconnection_cost=500000,
        annual_om_per_kw=10,
        variable_om_per_mwh=2.50,
        warranty_pct=0.009,
        warranty_start_year=3,
        capacity_credit_pct=0.95,
        capacity_price_per_kw_yr=50,
        freq_reg_capable=True,
        spinning_reserve_capable=True,
        black_start_capable=False,
        annual_insurance_pct=0.004,
        annual_property_tax_pct=0.0084,
        annual_land_lease=50000,
        decommissioning_cost_pct=0.05,
    )
    
    optimizer = DispatchOptimizer(battery)
    
    # Create minimal energy prices
    timestamps = [f"2024-01-01T{h:02d}:00:00" for h in range(24)]
    prices = [float(i) for i in range(24)]
    energy_prices = EnergyPrices(
        timestamps=timestamps,
        price_per_mwh=prices,
        year=2024,
        escalation_rate_pct=2.0,
    )
    
    # Create capacity market
    capacity_market = CapacityMarket(
        capacity_price_per_kw_yr=50,
        min_duration_hours=4,
        elcc_method="marginal",
        delivery_year_lead=3,
    )
    
    # Run optimization
    result = optimizer.optimize(
        energy_prices=energy_prices,
        ancillary_prices=None,
        capacity_market=capacity_market,
        year=2024,
    )
    
    assert isinstance(result, DispatchYear)
    assert len(result.hours) == 24


def test_hourly_dispatch_structure():
    """Test that hourly dispatch results have correct structure."""
    battery = BatterySpec(
        chemistry="LFP",
        power_mw=10.0,
        energy_mwh=20.0,
        duration_hours=2.0,
        roundtrip_efficiency=0.87,
        depth_of_discharge=0.90,
        min_soc=0.10,
        max_soc=1.00,
        max_cycles_per_day=2,
        calendar_life_years=20,
        cycle_life_cycles=6000,
        annual_degradation_pct=2.0,
        augmentation_year=10,
        augmentation_cost_pct=30,
        cost_per_kw=350,
        cost_per_kwh=250,
        epc_cost_pct=15,
        interconnection_cost=500000,
        annual_om_per_kw=10,
        variable_om_per_mwh=2.50,
        warranty_pct=0.009,
        warranty_start_year=3,
        capacity_credit_pct=0.95,
        capacity_price_per_kw_yr=50,
        freq_reg_capable=True,
        spinning_reserve_capable=True,
        black_start_capable=False,
        annual_insurance_pct=0.004,
        annual_property_tax_pct=0.0084,
        annual_land_lease=50000,
        decommissioning_cost_pct=0.05,
    )
    
    optimizer = DispatchOptimizer(battery)
    
    timestamps = [f"2024-01-01T{h:02d}:00:00" for h in range(24)]
    prices = [float(i) for i in range(24)]
    energy_prices = EnergyPrices(
        timestamps=timestamps,
        price_per_mwh=prices,
        year=2024,
        escalation_rate_pct=2.0,
    )
    
    result = optimizer.optimize(energy_prices=energy_prices, year=2024)
    
    # Check first hourly dispatch
    hourly = result.hours[0]
    assert isinstance(hourly, HourlyDispatch)
    assert hasattr(hourly, 'timestamp')
    assert hasattr(hourly, 'hour')
    assert hasattr(hourly, 'load_mw')
    assert hasattr(hourly, 'capacity_mw')
    assert hasattr(hourly, 'overload_mw')
    assert hasattr(hourly, 'battery_charge_mw')
    assert hasattr(hourly, 'battery_discharge_mw')
    assert hasattr(hourly, 'battery_soc_mwh')
    assert hasattr(hourly, 'battery_soc_pct')
    assert hasattr(hourly, 'ice_dispatch_mw')
    assert hasattr(hourly, 'customer_curtailment_mw')
    assert hasattr(hourly, 'unserved_mw')
    assert hasattr(hourly, 'energy_price_per_mwh')
    assert hasattr(hourly, 'arbitrage_revenue')
    assert hasattr(hourly, 'charging_cost')
    assert hasattr(hourly, 'fuel_cost')
    assert hasattr(hourly, 'carbon_cost')
    assert hasattr(hourly, 'net_hourly_value')


if __name__ == "__main__":
    import pytest
    pytest.main([__file__, "-v"])
