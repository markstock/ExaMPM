/****************************************************************************
 * Copyright (c) 2018-2020 by the ExaMPM authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the ExaMPM library. ExaMPM is distributed under a   *
 * BSD 3-clause license. For the licensing terms see the LICENSE file in    *
 * the top-level directory.                                                 *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#ifndef EXAMPM_SOLVER_HPP
#define EXAMPM_SOLVER_HPP

#include <ExaMPM_BoundaryConditions.hpp>
#include <ExaMPM_Mesh.hpp>
#include <ExaMPM_ProblemManager.hpp>
#include <ExaMPM_TimeIntegrator.hpp>

#include <Cabana_Core.hpp>
#include <Kokkos_Core.hpp>

#include <memory>
#include <string>

#include <mpi.h>

namespace ExaMPM
{
//---------------------------------------------------------------------------//
class SolverBase
{
  public:
    virtual ~SolverBase() = default;
    virtual void solve( const double t_final, const int write_freq ) = 0;
};

//---------------------------------------------------------------------------//
template <class MemorySpace, class ExecutionSpace>
class Solver : public SolverBase
{
  public:
    template <class InitFunc>
    Solver( MPI_Comm comm, const Kokkos::Array<double, 6>& global_bounding_box,
            const std::array<int, 3>& global_num_cell,
            const std::array<bool, 3>& periodic,
            const Cajita::BlockPartitioner<3>& partitioner,
            const int halo_cell_width, const InitFunc& create_functor,
            const int particles_per_cell, const double bulk_modulus,
            const double density, const double gamma, const double kappa,
            const double delta_t, const double gravity,
            const BoundaryCondition& bc )
        : _dt( delta_t )
        , _gravity( gravity )
        , _bc( bc )
        , _halo_min( 3 )
    {
        _mesh = std::make_shared<Mesh<MemorySpace>>(
            global_bounding_box, global_num_cell, periodic, partitioner,
            halo_cell_width, _halo_min, comm );

        _bc.min = _mesh->minDomainGlobalNodeIndex();
        _bc.max = _mesh->maxDomainGlobalNodeIndex();

        _pm = std::make_shared<ProblemManager<MemorySpace>>(
            ExecutionSpace(), _mesh, create_functor, particles_per_cell,
            bulk_modulus, density, gamma, kappa );

        MPI_Comm_rank( comm, &_rank );
    }

    void solve( const double t_final, const int write_freq ) override
    {
        // Output initial state.
        outputParticles( 0, 0.0 );

        int num_step = t_final / _dt;
        double delta_t = t_final / num_step;
        double time = 0.0;
        for ( int t = 0; t < num_step; ++t )
        {
            if ( 0 == _rank && 0 == t % write_freq )
                printf( "Step %d / %d\n", t, num_step );

            TimeIntegrator::step( ExecutionSpace(), *_pm, delta_t, _gravity,
                                  _bc );

            _pm->communicateParticles( _halo_min );

            // Output particles periodically.
            if ( 0 == ( t + 1 ) % write_freq )
                outputParticles( t + 1, time );

            time += delta_t;
        }
    }

    void outputParticles( const int step, const double time )
    {
        // Prefer HDF5 output over Silo. Only output if one is enabled.
#ifdef Cabana_ENABLE_HDF5
        Cabana::Experimental::HDF5ParticleOutput::HDF5Config h5_config;
        Cabana::Experimental::HDF5ParticleOutput::writeTimeStep(
            h5_config, "particles", _mesh->localGrid()->globalGrid().comm(),
            step, time, _pm->numParticle(),
            _pm->get( Location::Particle(), Field::Position() ),
            _pm->get( Location::Particle(), Field::Velocity() ),
            _pm->get( Location::Particle(), Field::J() ) );
#else
#ifdef Cabana_ENABLE_SILO
        Cajita::Experimental::SiloParticleOutput::writeTimeStep(
            "particles", _mesh->localGrid()->globalGrid(), step, time,
            _pm->get( Location::Particle(), Field::Position() ),
            _pm->get( Location::Particle(), Field::Velocity() ),
            _pm->get( Location::Particle(), Field::J() ) );
#endif
#endif
    }

  private:
    double _dt;
    double _gravity;
    BoundaryCondition _bc;
    int _halo_min;
    std::shared_ptr<Mesh<MemorySpace>> _mesh;
    std::shared_ptr<ProblemManager<MemorySpace>> _pm;
    int _rank;
};

//---------------------------------------------------------------------------//
// Creation method.
template <class InitFunc>
std::shared_ptr<SolverBase>
createSolver( const std::string& device, MPI_Comm comm,
              const Kokkos::Array<double, 6>& global_bounding_box,
              const std::array<int, 3>& global_num_cell,
              const std::array<bool, 3>& periodic,
              const Cajita::BlockPartitioner<3>& partitioner,
              const int halo_cell_width, const InitFunc& create_functor,
              const int particles_per_cell, const double bulk_modulus,
              const double density, const double gamma, const double kappa,
              const double delta_t, const double gravity,
              const BoundaryCondition& bc )
{
    if ( 0 == device.compare( "serial" ) )
    {
#ifdef KOKKOS_ENABLE_SERIAL
        return std::make_shared<
            ExaMPM::Solver<Kokkos::HostSpace, Kokkos::Serial>>(
            comm, global_bounding_box, global_num_cell, periodic, partitioner,
            halo_cell_width, create_functor, particles_per_cell, bulk_modulus,
            density, gamma, kappa, delta_t, gravity, bc );
#else
        throw std::runtime_error( "Serial Backend Not Enabled" );
#endif
    }
    else if ( 0 == device.compare( "openmp" ) )
    {
#ifdef KOKKOS_ENABLE_OPENMP
        return std::make_shared<
            ExaMPM::Solver<Kokkos::HostSpace, Kokkos::OpenMP>>(
            comm, global_bounding_box, global_num_cell, periodic, partitioner,
            halo_cell_width, create_functor, particles_per_cell, bulk_modulus,
            density, gamma, kappa, delta_t, gravity, bc );
#else
        throw std::runtime_error( "OpenMP Backend Not Enabled" );
#endif
    }
    else if ( 0 == device.compare( "cuda" ) )
    {
#ifdef KOKKOS_ENABLE_CUDA
        return std::make_shared<
            ExaMPM::Solver<Kokkos::CudaSpace, Kokkos::Cuda>>(
            comm, global_bounding_box, global_num_cell, periodic, partitioner,
            halo_cell_width, create_functor, particles_per_cell, bulk_modulus,
            density, gamma, kappa, delta_t, gravity, bc );
#else
        throw std::runtime_error( "CUDA Backend Not Enabled" );
#endif
    }
    else if ( 0 == device.compare( "hip" ) )
    {
#ifdef KOKKOS_ENABLE_HIP
        return std::make_shared<ExaMPM::Solver<Kokkos::Experimental::HIPSpace,
                                               Kokkos::Experimental::HIP>>(
            comm, global_bounding_box, global_num_cell, periodic, partitioner,
            halo_cell_width, create_functor, particles_per_cell, bulk_modulus,
            density, gamma, kappa, delta_t, gravity, bc );
#else
        throw std::runtime_error( "HIP Backend Not Enabled" );
#endif
    }
    else
    {
        throw std::runtime_error( "invalid backend" );
        return nullptr;
    }
}

//---------------------------------------------------------------------------//

} // end namespace ExaMPM

#endif // end EXAMPM_SOLVER_HPP
