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
import { ThemeProvider, createTheme, StyledEngineProvider, Theme } from '@mui/material/styles';

import AppThemed from "./AppThemed";
import { isDarkMode } from './DarkMode';

declare module '@mui/material/styles' {
    interface Theme {
        mainBackground: React.CSSProperties['color'];
        toolbarColor: React.CSSProperties['color'];
    }
    interface ThemeOptions {
        mainBackground?: React.CSSProperties['color'];
        toolbarColor?: React.CSSProperties['color'];
    }

}

declare module '@mui/material/Button' {
    interface ButtonPropsVariantOverrides {
        dialogPrimary: true;
        dialogSecondary: true;
    }
}


declare module '@mui/styles/defaultTheme' {
    // eslint-disable-next-line @typescript-eslint/no-empty-interface
    interface DefaultTheme extends Theme { }
}




const theme = createTheme(
    isDarkMode() ?
        {
            components: {
                MuiButton: {
                    variants: [
                        {
                            props: { variant: 'dialogPrimary' },
                            style: {
                                color: "#FFFFFF"
                            }
                        },
                        {
                            props: { variant: 'dialogSecondary',  },
                            style: {
                                color: "rgb(255,255,255,0.7)"
                            },
                        },
                    ],
                },
            },

            palette: {
                mode: 'dark',
                primary: {
                    main: '#A770E4'// #6750A4"   // #5B5690  #60529A  #5C5694
                },
                secondary: {
                    main: "#0AA102" //'"#FF6060"
                },
            },
            mainBackground: "#222",
            toolbarColor: '#222'
        }
        :
        {
            components: {
                MuiButton: {
                    variants: [
                        {
                            props: { variant: 'dialogPrimary' },
                            style: {
                                color: "rgb(0,0,0,0.87)"
                            }
                        },
                        {
                            props: { variant: 'dialogSecondary',  },
                            style: {
                                color: "rgb(0,0,0,0.6)"
                            },
                        },
                    ],
                },
            },
            palette: {
                primary: {
                    main: "#6750A4"   // #5B5690  #60529A  #5C5694
                },
                secondary: {
                    main: "#FF6060"
                }

            },
            mainBackground: "#FFFFFF",
            toolbarColor: '#FFFFFF'


        }
);



type AppThemeProps = {

};
const App = (class extends React.Component {
    // Before the component mounts, we initialise our state

    constructor(props: AppThemeProps) {
        super(props);
        this.state = {
        };
    }

    render() {
        return (
            <StyledEngineProvider injectFirst>
                <ThemeProvider theme={theme}>
                    <AppThemed />
                </ThemeProvider>
            </StyledEngineProvider>
        );
    }
}
);

export default App;
