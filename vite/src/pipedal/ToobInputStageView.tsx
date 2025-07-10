// Copyright (c) 2022 Robin Davies
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

import React from 'react';
import { Theme } from '@mui/material/styles';

import WithStyles from './WithStyles';
import { createStyles } from './WithStyles';

import { withStyles } from "tss-react/mui";

import IControlViewFactory from './IControlViewFactory';
import { PiPedalModelFactory, PiPedalModel } from "./PiPedalModel";
import { PedalboardItem } from './Pedalboard';
import PluginControlView, { ControlGroup, ControlViewCustomization } from './PluginControlView';
//import ToobFrequencyResponseView from './ToobFrequencyResponseView';



const styles = (theme: Theme) => createStyles({
});

interface ToobInputStageProps extends WithStyles<typeof styles> {
    instanceId: number;
    item: PedalboardItem;

}
interface ToobInputStageState {

}

const ToobInputStageView =
    withStyles(
        class extends React.Component<ToobInputStageProps, ToobInputStageState>
            implements ControlViewCustomization {
            model: PiPedalModel;

            customizationId: number = 1;

            constructor(props: ToobInputStageProps) {
                super(props);
                this.model = PiPedalModelFactory.getInstance();
                this.state = {
                }
            }
            fullScreen() {
                return false;
            }

            modifyControls(controls: (React.ReactNode | ControlGroup)[]): (React.ReactNode | ControlGroup)[] {
                return controls;
                // let group = controls[1] as ControlGroup;
                // group.controls.splice(0,0,
                //     ( <ToobFrequencyResponseView instanceId={this.props.instanceId} />)
                //     );
                // return controls;
            }
            render() {
                return (<PluginControlView
                    instanceId={this.props.instanceId}
                    item={this.props.item}
                    customization={this}
                    customizationId={this.customizationId}
                    showModGui={false}
                    onSetShowModGui={(instanceId: number, showModGui: boolean) => { }}

                />);
            }
        },
        styles
    );



class ToobInputStageViewFactory implements IControlViewFactory {
    uri: string = "http://two-play.com/plugins/toob-input_stage";

    Create(model: PiPedalModel, pedalboardItem: PedalboardItem): React.ReactNode {
        return (<ToobInputStageView instanceId={pedalboardItem.instanceId} item={pedalboardItem} />);
    }


}
export default ToobInputStageViewFactory;