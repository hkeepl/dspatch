#pragma once

#include <iostream>

namespace DSPatch
{

class Incrementer : public Component
{
public:
    explicit Incrementer( int increment = 1 )
        : Component( ProcessOrder::OutOfOrder )
        , _increment( increment )
    {
        SetInputCount_( 1 );
        SetOutputCount_( 1 );
    }

protected:
    virtual void Process_( SignalBus& inputs, SignalBus& outputs ) override
    {
        auto in = inputs.GetValue<int>( 0 );
        if ( in )
        {
            *in += _increment;
            outputs.MoveValue( 0, std::move( *in ) );  // pass the adjusted signal through (no copy)
        }
        // else set no output
    }

private:
    int _increment;
};

}  // namespace DSPatch
