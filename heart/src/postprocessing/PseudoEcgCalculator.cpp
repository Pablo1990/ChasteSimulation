/*

Copyright (c) 2005-2016, University of Oxford.
All rights reserved.

University of Oxford means the Chancellor, Masters and Scholars of the
University of Oxford, having an administrative office at Wellington
Square, Oxford OX1 2JD, UK.

This file is part of Chaste.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
 * Neither the name of the University of Oxford nor the names of its
   contributors may be used to endorse or promote products derived from this
   software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "PseudoEcgCalculator.hpp"
#include "HeartRegionCodes.hpp"
#include "HeartConfig.hpp"
#include "PetscTools.hpp"
#include "Version.hpp"
#include <iostream>

template<unsigned ELEMENT_DIM, unsigned SPACE_DIM, unsigned PROBLEM_DIM>
double PseudoEcgCalculator<ELEMENT_DIM, SPACE_DIM, PROBLEM_DIM> ::GetIntegrand(ChastePoint<SPACE_DIM>& rX,
                                c_vector<double,PROBLEM_DIM>& rU,
                                c_matrix<double,PROBLEM_DIM,SPACE_DIM>& rGradU)
{
    c_vector<double,SPACE_DIM> r_vector = rX.rGetLocation()- mProbeElectrode.rGetLocation();
    double norm_r = norm_2(r_vector);
    if (norm_r <= DBL_EPSILON)
    {
        EXCEPTION("Probe is on a mesh Gauss point.");
    }
    c_vector<double,SPACE_DIM> grad_one_over_r = - (r_vector)*SmallPow( (1/norm_r) , 3);
    matrix_row<c_matrix<double, PROBLEM_DIM, SPACE_DIM> > grad_u_row(rGradU, 0);
    double integrand = inner_prod(grad_u_row, grad_one_over_r);

    return -mDiffusionCoefficient*integrand;
}

template<unsigned ELEMENT_DIM, unsigned SPACE_DIM, unsigned PROBLEM_DIM>
PseudoEcgCalculator<ELEMENT_DIM, SPACE_DIM, PROBLEM_DIM>::PseudoEcgCalculator(AbstractTetrahedralMesh<ELEMENT_DIM,SPACE_DIM>& rMesh,
                                                                              const ChastePoint<SPACE_DIM>& rProbeElectrode,
                                                                              const FileFinder& rDirectory,
                                                                              const std::string& rHdf5FileName,
                                                                              const std::string& rVariableName,
                                                                              unsigned timestepStride)
  : mrMesh(rMesh),
    mProbeElectrode(rProbeElectrode),
    mVariableName(rVariableName),
    mTimestepStride(timestepStride)
{
    mpDataReader = new Hdf5DataReader(rDirectory, rHdf5FileName);
    mNumberOfNodes = mpDataReader->GetNumberOfRows();
    mNumTimeSteps = mpDataReader->GetVariableOverTime(mVariableName, 0u).size();
    mDiffusionCoefficient = 1.0;
    //check that the hdf file was generated by simulations from the same mesh
    assert(mNumberOfNodes == mrMesh.GetNumNodes());
}

template<unsigned ELEMENT_DIM, unsigned SPACE_DIM, unsigned PROBLEM_DIM>
bool PseudoEcgCalculator<ELEMENT_DIM, SPACE_DIM, PROBLEM_DIM>::ShouldSkipThisElement(Element<ELEMENT_DIM,SPACE_DIM>& rElement)
{
    bool result = false; // Don't skip usually
    if (mVariableName=="V")
    {
        // If we are integrating voltage and this element is in the bath then skip it.
        result = HeartRegionCode::IsRegionBath(rElement.GetUnsignedAttribute());
    }
    return result;
}

template<unsigned ELEMENT_DIM, unsigned SPACE_DIM, unsigned PROBLEM_DIM>
PseudoEcgCalculator<ELEMENT_DIM, SPACE_DIM, PROBLEM_DIM>::~PseudoEcgCalculator()
{
    delete mpDataReader;
}

template<unsigned ELEMENT_DIM, unsigned SPACE_DIM, unsigned PROBLEM_DIM>
void PseudoEcgCalculator<ELEMENT_DIM, SPACE_DIM, PROBLEM_DIM>::SetDiffusionCoefficient(double diffusionCoefficient)
{
    assert(diffusionCoefficient>=0);
    mDiffusionCoefficient = diffusionCoefficient;
}

template<unsigned ELEMENT_DIM, unsigned SPACE_DIM, unsigned PROBLEM_DIM>
double PseudoEcgCalculator<ELEMENT_DIM, SPACE_DIM, PROBLEM_DIM>::ComputePseudoEcgAtOneTimeStep (unsigned timeStep)
{
    Vec solution_at_one_time_step = PetscTools::CreateVec(mNumberOfNodes);
    mpDataReader->GetVariableOverNodes(solution_at_one_time_step, mVariableName , timeStep);

    double pseudo_ecg_at_one_timestep;
    try
    {
        pseudo_ecg_at_one_timestep = this->Calculate(mrMesh, solution_at_one_time_step);
    }
    catch (Exception &e)
    {
        PetscTools::Destroy(solution_at_one_time_step);
        throw e;
    }
    PetscTools::Destroy(solution_at_one_time_step);
    return pseudo_ecg_at_one_timestep;
}

template<unsigned ELEMENT_DIM, unsigned SPACE_DIM, unsigned PROBLEM_DIM>
void PseudoEcgCalculator<ELEMENT_DIM, SPACE_DIM, PROBLEM_DIM>::WritePseudoEcg ()
{
    // Cache the time values so that we can plot a decent x-axis
    std::vector<double> time_points = mpDataReader->GetUnlimitedDimensionValues();

    std::stringstream stream;
    stream << "PseudoEcgFromElectrodeAt" << "_" << mProbeElectrode.GetWithDefault(0)
                          << "_" << mProbeElectrode.GetWithDefault(1)
                          << "_" << mProbeElectrode.GetWithDefault(2) << ".dat";
    OutputFileHandler output_file_handler(HeartConfig::Instance()->GetOutputDirectory() + "/output", false);
    out_stream p_file=out_stream(NULL);
    if (PetscTools::AmMaster())
    {
        p_file = output_file_handler.OpenOutputFile(stream.str());
        *p_file << "#Time(ms)\tPseudo-ECG\n";
    }
    for (unsigned time_step = 0; time_step < mNumTimeSteps; time_step+=mTimestepStride)
    {
        double pseudo_ecg_at_one_timestep = ComputePseudoEcgAtOneTimeStep(time_step);
        if (PetscTools::AmMaster())
        {
            *p_file << time_points[time_step] << "\t" <<pseudo_ecg_at_one_timestep << "\n";
        }
    }
    if (PetscTools::AmMaster())
    {
        //write provenance info
        std::string comment = "# " + ChasteBuildInfo::GetProvenanceString();
        *p_file << comment;
        p_file->close();
    }
}
/////////////////////////////////////////////////////////////////////
// Explicit instantiation
/////////////////////////////////////////////////////////////////////

template class PseudoEcgCalculator<1,1,1>;
template class PseudoEcgCalculator<1,2,1>;
template class PseudoEcgCalculator<1,3,1>;
template class PseudoEcgCalculator<2,2,1>;
//template class PseudoEcgCalculator<2,3,1>;
template class PseudoEcgCalculator<3,3,1>;
