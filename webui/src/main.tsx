import { render } from 'preact';
import { App } from './App';
import { injectKsuInsets } from './bridge';
import './styles.css';

injectKsuInsets();
render(<App />, document.getElementById('app')!);
