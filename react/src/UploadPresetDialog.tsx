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
import Button from '@mui/material/Button';
import {PiPedalModel,PiPedalModelFactory} from './PiPedalModel';

import DialogEx from './DialogEx';
import DialogTitle from '@mui/material/DialogTitle';
import DialogActions from '@mui/material/DialogActions';
import DialogContent from '@mui/material/DialogContent';
import ResizeResponsiveComponent from './ResizeResponsiveComponent';
import Typography from '@mui/material/Typography';


// const PRESET_EXTENSION = ".piPreset";
// const BANK_EXTENSION = ".piBank";
// const PLUGIN_PRESETS_EXTENSION = ".piPluginPresets";

export interface UploadPresetDialogProps {
    open: boolean,
    onClose: () => void,
    uploadAfter: number,
    onUploaded: (instanceId: number) => void,
    title: string,
    extension: string,
    uploadPage: string

};

export interface UploadPresetDialogState {
    fullScreen: boolean;
};

export default class UploadPresetDialog extends ResizeResponsiveComponent<UploadPresetDialogProps, UploadPresetDialogState> {

    model: PiPedalModel;

    constructor(props: UploadPresetDialogProps) {
        super(props);
        this.state = {
            fullScreen: false
        };
        this.model = PiPedalModelFactory.getInstance();

    }
    mounted: boolean = false;



    onWindowSizeChanged(width: number, height: number): void {
        this.setState({ fullScreen: height < 200 })
    }


    componentDidMount() {
        super.componentDidMount();
        this.mounted = true;
        
    }
    componentWillUnmount() {
        super.componentWillUnmount();
        this.mounted = false;
        
    }

    componentDidUpdate() {
        
    }

    handleClose(): void {
        this.props.onClose();
    }

    handleOk(): void {
        this.props.onClose();
    }

    async uploadFiles(fileList: FileList)
    {
        try {
            let uploadAfter = this.props.uploadAfter;
            for (let i = 0; i < fileList.length; ++i)
            {
                uploadAfter = await this.model.uploadPreset(this.props.uploadPage,fileList[i],uploadAfter);
            }
            this.props.onUploaded(uploadAfter);
        } catch(error) {
            this.model.showAlert(error +"");
        };
        this.props.onClose();
    }
    handleDrop(e: React.DragEvent<HTMLDivElement>) {
        e.preventDefault();
        e.currentTarget.style.background = "";

        if (this.wantsTransfer(e.dataTransfer)) {
            this.uploadFiles(e.dataTransfer.files)
            .then(() => {
                this.handleClose();
            })
            .catch((error)=> {
                this.handleClose();
                this.model.showAlert(error);
            });
        }
    }

    getFileExtension(name: string): string {
        let pos = name.lastIndexOf('.');
        if (pos !== -1) {
            return name.substring(pos);
        }
        return "";
    }

    wantsTransfer(fileList: DataTransfer): boolean {
        if (fileList.files.length === 0) return false;

        for (let i = 0; i < fileList.files.length; ++i) {
            let file = fileList.files[i];
            let extension = this.getFileExtension(file.name);
            if (extension !== this.props.extension) return false;
        }
        return true;
    }
    handleDragOver(e: React.DragEvent<HTMLDivElement>) {
        e.currentTarget.style.background = "#D8D8D8";
        e.preventDefault();
    }

    handleButtonSelect(e: React.ChangeEvent<HTMLInputElement>)
    {
        if (e.currentTarget.files)
        {
            this.uploadFiles(e.currentTarget.files);
        }
    }

    render() {


        return (
            <DialogEx tag="uploadPreset" open={this.props.open} fullWidth onClose={() => this.handleClose()} 
                fullScreen={this.state.fullScreen}
                style={{userSelect: "none"}}
            >
                <DialogTitle>{this.props.title}</DialogTitle>
                <DialogContent>
                    <div style={{
                        width: "100%", height: 100, marginBottom: 0,
                        border: "2px dashed #CCC",
                        borderRadius: "10px",
                        fontFamily: "Roboto",
                        padding: 20
                    }}
                        onDragEnter={(e) => { this.handleDragOver(e); }}
                        onDragLeave={(e) => { e.currentTarget.style.background = ""; e.preventDefault(); }}
                        onDragOver={(e) => { this.handleDragOver(e); }}
                        onDrop={(e) => {
                            this.handleDrop(e);
                        }}
                    >
                        <Typography  noWrap color="textSecondary" align="center" variant="caption" style={{verticalAlign: "middle"}} >Drop files here</Typography>
                    </div>
                </DialogContent>

                <DialogActions>
                    <Button onClick={() => this.handleClose()} color="primary">
                        Cancel
                    </Button>
                    <Button
                        component="label" color="secondary" style={{width: 120}}
                    >
                        Select File
                        <input
                            type="file"
                            hidden
                            accept={this.props.extension}
                            multiple
                            onChange={(e)=> this.handleButtonSelect(e)}
                        />
                    </Button>
                </DialogActions>
            </DialogEx>
        );
    }
}