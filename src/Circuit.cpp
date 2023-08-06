/******************************************************************************
DSPatch - The Refreshingly Simple C++ Dataflow Framework
Copyright (c) 2023, Marcus Tomlinson

BSD 2-Clause License

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include <dspatch/Circuit.h>

#include <internal/AutoTickThread.h>
#include <internal/CircuitThread.h>

#include <algorithm>
#include <unordered_set>

using namespace DSPatch;

namespace DSPatch
{
namespace internal
{

class Circuit
{
public:
    int FindComponent( const DSPatch::Component::SPtr& component ) const;

    int pauseCount = 0;
    size_t currentThreadNo = 0;

    AutoTickThread autoTickThread;

    std::unordered_set<DSPatch::Component::SPtr> componentsSet;
    std::vector<DSPatch::Component::SPtr> components;

    std::vector<CircuitThread> circuitThreads;
};

}  // namespace internal
}  // namespace DSPatch

Circuit::Circuit()
    : p( new internal::Circuit() )
{
}

Circuit::~Circuit()
{
    StopAutoTick();
    SetBufferCount( 0 );
    RemoveAllComponents();

    delete p;
}

bool Circuit::AddComponent( const Component::SPtr& component )
{
    if ( !component || p->componentsSet.find( component ) != p->componentsSet.end() )
    {
        return false;
    }

    // components within the circuit need to have as many buffers as there are threads in the circuit
    component->SetBufferCount( (int)p->circuitThreads.size() );

    p->componentsSet.emplace( component );

    PauseAutoTick();
    p->components.emplace_back( component );
    ResumeAutoTick();

    return true;
}

bool Circuit::RemoveComponent( const Component::SPtr& component )
{
    auto componentIndex = p->FindComponent( component );

    if ( componentIndex != -1 )
    {
        p->componentsSet.erase( p->components[componentIndex] );

        PauseAutoTick();

        DisconnectComponent( component );

        p->components.erase( p->components.begin() + componentIndex );

        ResumeAutoTick();

        return true;
    }

    return false;
}

void Circuit::RemoveAllComponents()
{
    PauseAutoTick();

    DisconnectAllComponents();

    p->componentsSet.clear();
    p->components.clear();

    ResumeAutoTick();
}

// cppcheck-suppress unusedFunction
int Circuit::GetComponentCount() const
{
    return (int)p->components.size();
}

bool Circuit::ConnectOutToIn( const Component::SPtr& fromComponent, int fromOutput, const Component::SPtr& toComponent, int toInput )
{
    if ( p->componentsSet.find( fromComponent ) == p->componentsSet.end() ||
         p->componentsSet.find( toComponent ) == p->componentsSet.end() )
    {
        return false;
    }

    PauseAutoTick();
    bool result = toComponent->ConnectInput( fromComponent, fromOutput, toInput );
    ResumeAutoTick();

    return result;
}

bool Circuit::DisconnectComponent( const Component::SPtr& component )
{
    if ( p->componentsSet.find( component ) == p->componentsSet.end() )
    {
        return false;
    }

    PauseAutoTick();

    // remove component from _inputComponents and _inputWires
    component->DisconnectAllInputs();

    // remove any connections this component has to other components
    for ( auto& comp : p->components )
    {
        comp->DisconnectInput( component );
    }

    ResumeAutoTick();

    return true;
}

void Circuit::DisconnectAllComponents()
{
    PauseAutoTick();

    for ( auto& component : p->components )
    {
        component->DisconnectAllInputs();
    }

    ResumeAutoTick();
}

void Circuit::SetBufferCount( int bufferCount )
{
    if ( (size_t)bufferCount != p->circuitThreads.size() )
    {
        PauseAutoTick();

        // stop all threads
        for ( auto& circuitThread : p->circuitThreads )
        {
            circuitThread.Stop();
        }

        // resize thread array
        p->circuitThreads.resize( bufferCount );

        // initialise and start all threads
        for ( size_t i = 0; i < p->circuitThreads.size(); ++i )
        {
            p->circuitThreads[i].Start( &p->components, (int)i );
        }

        // set all components to the new buffer count
        for ( auto& component : p->components )
        {
            component->SetBufferCount( bufferCount );
        }

        p->currentThreadNo = 0;

        ResumeAutoTick();
    }
}

int Circuit::GetBufferCount() const
{
    return (int)p->circuitThreads.size();
}

void Circuit::Tick( Component::TickMode mode )
{
    // process in a single thread if this circuit has no threads
    // =========================================================
    if ( p->circuitThreads.empty() )
    {
        // tick all internal components
        for ( auto& component : p->components )
        {
            component->Tick( mode );
        }

        // reset all internal components
        for ( auto& component : p->components )
        {
            component->Reset( mode );
        }
    }
    // process in multiple threads if this circuit has threads
    // =======================================================
    else
    {
        p->circuitThreads[p->currentThreadNo].SyncAndResume( mode );  // sync and resume thread x

        if ( ++p->currentThreadNo == p->circuitThreads.size() )
        {
            p->currentThreadNo = 0;
        }
    }
}

void Circuit::StartAutoTick( Component::TickMode mode )
{
    if ( p->autoTickThread.IsStopped() )
    {
        p->autoTickThread.Start( this, mode );
    }
    else
    {
        ResumeAutoTick();
    }
}

void Circuit::StopAutoTick()
{
    if ( !p->autoTickThread.IsStopped() )
    {
        p->autoTickThread.Stop();

        // manually tick until 0
        while ( p->currentThreadNo != 0 )
        {
            Tick( p->autoTickThread.Mode() );
        }

        // sync all threads
        for ( auto& circuitThread : p->circuitThreads )
        {
            circuitThread.Sync();
        }
    }
}

void Circuit::PauseAutoTick()
{
    if ( p->autoTickThread.IsStopped() )
    {
        return;
    }

    if ( ++p->pauseCount == 1 && !p->autoTickThread.IsPaused() )
    {
        p->autoTickThread.Pause();

        // manually tick until 0
        while ( p->currentThreadNo != 0 )
        {
            Tick( p->autoTickThread.Mode() );
        }

        // sync all threads
        for ( auto& circuitThread : p->circuitThreads )
        {
            circuitThread.Sync();
        }
    }
}

void Circuit::ResumeAutoTick()
{
    if ( p->autoTickThread.IsPaused() && --p->pauseCount == 0 )
    {
        p->autoTickThread.Resume();
    }
}

int internal::Circuit::FindComponent( const DSPatch::Component::SPtr& component ) const
{
    if ( componentsSet.find( component ) == componentsSet.end() )
    {
        return -1;
    }

    auto findFn = [&component]( const auto& comp ) { return comp == component; };

    if ( auto it = std::find_if( components.begin(), components.end(), findFn ); it != components.end() )
    {
        return (int)( it - components.begin() );
    }

    return -1;
}
